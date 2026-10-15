#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// Work as a graph of queues that workers pull from, in the style of SARP's static streaming (its work sources, claims
// and blocking-IO pool) rather than a sequence of parallel loops. A node is a queue of typed items with one handler; a
// job is a node of one item. Edges say when a node's items may run (After) and when a node has had all its items
// (Feeds: a node fed by others closes when they complete). Workers claim the best runnable item across nodes, in a
// weighted fair order with a cap per node, on a TBB arena (the Cpu lane) or a few threads of their own that may block
// (the Io lane). Items may add items, and nodes, while the graph runs.
namespace tesworld::jobs
{
	enum class Lane
	{
		Cpu,
		// Blocking work (writing files): threads beyond the cores, so a wait on the disk doesn't idle one.
		Io
	};

	struct NodeOptions
	{
		// A node's share when several have items: claims go in proportion to priority (stride scheduling).
		std::uint32_t priority{ 1 };
		// Items of this node running at once; 0 for no cap.
		std::uint32_t maxParallel{ 0 };
		Lane lane{ Lane::Cpu };
		// Its share of the graph's progress.
		double weight{ 1.0 };
	};

	struct GraphOptions
	{
		// Cpu workers; 0 for TBB's default concurrency.
		std::uint32_t threads{ 0 };
		std::uint32_t ioThreads{ 4 };
		// Polled as items are claimed: once true, no more are, and Run returns when those running are done.
		std::function<bool()> cancelled;
		// The graph's fraction done and the node an item was last claimed from, every so often.
		std::function<void(double, const std::string&)> progress;
	};

	class Graph;
	class Node;

	class JobContext
	{
	public:
		[[nodiscard]] bool Cancelled() const;
		[[nodiscard]] Graph& Owner() const { return _graph; }
		// The node the item is of: a job that hands work to nodes of its own holds itself (Holding) until they're done.
		[[nodiscard]] Node& Current() const { return _node; }

	private:
		friend class Graph;
		JobContext(Graph& a_graph, Node& a_node) :
			_graph(a_graph),
			_node(a_node)
		{}
		Graph& _graph;
		Node& _node;
	};

	// Keeps a node from completing while work it owes is elsewhere (a GPU encode whose callback will push items): while
	// it's held, items may be pushed to it even once it has closed.
	class Hold
	{
	public:
		Hold() = default;
		Hold(const Hold& a_other);
		Hold& operator=(const Hold& a_other);
		Hold(Hold&& a_other) noexcept;
		Hold& operator=(Hold&& a_other) noexcept;
		~Hold();

		void Release();

	private:
		friend class Node;
		explicit Hold(Node* a_node) :
			_node(a_node)
		{}
		Node* _node{ nullptr };
	};

	class Node
	{
	public:
		virtual ~Node() = default;
		Node(const Node&) = delete;
		Node& operator=(const Node&) = delete;

		// This node's items run once a_other has completed. Before the node has items, or before Run: a node made while
		// the graph runs takes what it waits on when it's made.
		Node& After(Node& a_other);
		// a_other closes once this node and every other node feeding it have completed.
		Node& Feeds(Node& a_other);
		// No more items will be pushed (implicit for a node others feed, once they complete).
		void Close();
		[[nodiscard]] Hold Holding();

		[[nodiscard]] const std::string& Name() const { return _name; }
		[[nodiscard]] bool Complete() const;

	protected:
		Node(Graph& a_graph, std::string a_name, NodeOptions a_options);
		// Called with the graph's lock held.
		void Pushed(std::size_t a_count);

		Graph& _graph;

	private:
		friend class Graph;
		friend class Hold;

		// The graph's lock is held for these.
		virtual bool Empty() const = 0;
		virtual std::function<void(JobContext&)> Take() = 0;
		virtual void Clear() = 0;
		// What its handler owns (a stage's state), let go when the graph goes: every node's at once, across the cores.
		// Not as each completes: memory freed while the run goes on is given back to the system and faulted in again.
		virtual void Release() = 0;

		std::string _name;
		NodeOptions _options;
		std::size_t _index{ 0 };
		std::size_t _waitingOn{ 0 };    // nodes After'd not yet complete
		std::size_t _openFeeders{ 0 };  // nodes feeding this one not yet complete
		bool _fed{ false };             // has feeders: closes when they complete
		bool _closed{ false };
		bool _complete{ false };
		std::size_t _active{ 0 };
		std::size_t _holds{ 0 };
		std::size_t _pushed{ 0 };
		std::size_t _done{ 0 };
		double _pass{ 0.0 };
		std::vector<Node*> _dependents;
		std::vector<Node*> _feeds;
	};

	template <class Item>
	class Queue final : public Node
	{
	public:
		using Handler = std::function<void(Item&, JobContext&)>;

		void Push(Item a_item);
		void PushRange(std::vector<Item> a_items);

	private:
		friend class Graph;
		Queue(Graph& a_graph, std::string a_name, Handler a_handler, NodeOptions a_options) :
			Node(a_graph, std::move(a_name), a_options),
			_handler(std::move(a_handler))
		{}

		bool Empty() const override { return _items.empty(); }
		std::function<void(JobContext&)> Take() override
		{
			auto item = std::make_shared<Item>(std::move(_items.front()));
			_items.pop_front();
			return [this, item](JobContext& a_context) { _handler(*item, a_context); };
		}
		void Clear() override { _items.clear(); }
		void Release() override
		{
			_handler = nullptr;
			std::deque<Item>().swap(_items);
		}

		Handler _handler;
		std::deque<Item> _items;
	};

	class Graph
	{
	public:
		explicit Graph(GraphOptions a_options = {});
		~Graph();
		Graph(const Graph&) = delete;
		Graph& operator=(const Graph&) = delete;

		// A queue of items for a_handler, its items run once a_after have completed. Nodes may be added while the graph
		// runs (from items).
		template <class Item>
		Queue<Item>& MakeQueue(std::string a_name, typename Queue<Item>::Handler a_handler, NodeOptions a_options = {}, std::vector<Node*> a_after = {})
		{
			auto node = std::unique_ptr<Queue<Item>>(new Queue<Item>(*this, std::move(a_name), std::move(a_handler), a_options));
			auto& queue = *node;
			Adopt(std::move(node), a_after);
			return queue;
		}

		// A node of one item, closed.
		Node& MakeJob(std::string a_name, std::function<void(JobContext&)> a_work, NodeOptions a_options = {}, std::vector<Node*> a_after = {});

		// Runs until every node has completed (or the graph is cancelled and what's running is done). The first exception
		// an item throws cancels the graph and is rethrown here. A node that can never complete (never closed, or waiting
		// on itself) is a logic_error, named, once nothing else can run.
		void Run();
		void Cancel();
		[[nodiscard]] bool Cancelled() const;
		// The graph's fraction done.
		[[nodiscard]] double Progress() const;

	private:
		friend class Node;
		template <class Item>
		friend class Queue;
		friend class Hold;

		struct Impl;
		struct Claim
		{
			Node* node{ nullptr };
			std::function<void(JobContext&)> work;
		};

		void Adopt(std::unique_ptr<Node> a_node, const std::vector<Node*>& a_after);
		void Execute(Claim a_claim);
		void Drain();
		void IoThread();
		// The graph's lock is held for these.
		static bool Claimable(const Node& a_node, Lane a_lane);
		void Runnable(Node& a_node);
		void MaybeComplete(Node& a_node);
		void Wake();
		Claim TryClaim(Lane a_lane);
		[[nodiscard]] std::string Stuck() const;
		[[nodiscard]] double ProgressLocked() const;

		std::unique_ptr<Impl> _impl;
		std::mutex& Mutex();
	};

	template <class Item>
	void Queue<Item>::Push(Item a_item)
	{
		std::scoped_lock lock(_graph.Mutex());
		_items.push_back(std::move(a_item));
		Pushed(1);
	}

	template <class Item>
	void Queue<Item>::PushRange(std::vector<Item> a_items)
	{
		if (a_items.empty()) return;
		std::scoped_lock lock(_graph.Mutex());
		const auto count = a_items.size();
		for (auto& item : a_items) _items.push_back(std::move(item));
		Pushed(count);
	}
}