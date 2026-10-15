#include "tesworld/Jobs.h"

#include "tesworld/Profiling.h"

#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/info.h>
#include <oneapi/tbb/task_arena.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>

namespace tesworld::jobs
{
	struct Graph::Impl
	{
		GraphOptions options;
		std::mutex mutex;
		// Run's thread waits on it for work or the end; the Io threads on ioWork.
		std::condition_variable changed;
		std::condition_variable ioWork;
		std::vector<std::unique_ptr<Node>> nodes;
		std::size_t incomplete{ 0 };
		std::size_t active{ 0 };
		std::size_t drains{ 0 };
		std::uint32_t workers{ 0 };
		std::optional<oneapi::tbb::task_arena> arena;
		std::vector<std::thread> io;
		bool running{ false };
		bool ioStopping{ false };
		std::atomic<bool> stopped{ false };
		std::exception_ptr error;
		double virtualTime{ 0.0 };
		std::chrono::steady_clock::time_point lastProgress{};
	};

	namespace
	{
		using Clock = std::chrono::steady_clock;
		constexpr auto kProgressEvery = std::chrono::milliseconds(50);
	}

	// ---- JobContext, Hold ----

	bool JobContext::Cancelled() const { return _graph.Cancelled(); }

	Hold::Hold(const Hold& a_other) :
		_node(a_other._node)
	{
		if (_node) {
			std::scoped_lock lock(_node->_graph.Mutex());
			++_node->_holds;
		}
	}

	Hold& Hold::operator=(const Hold& a_other)
	{
		if (this != &a_other) {
			Release();
			_node = a_other._node;
			if (_node) {
				std::scoped_lock lock(_node->_graph.Mutex());
				++_node->_holds;
			}
		}
		return *this;
	}

	Hold::Hold(Hold&& a_other) noexcept :
		_node(std::exchange(a_other._node, nullptr))
	{}

	Hold& Hold::operator=(Hold&& a_other) noexcept
	{
		if (this != &a_other) {
			Release();
			_node = std::exchange(a_other._node, nullptr);
		}
		return *this;
	}

	Hold::~Hold() { Release(); }

	void Hold::Release()
	{
		auto* node = std::exchange(_node, nullptr);
		if (!node) return;
		auto& graph = node->_graph;
		std::scoped_lock lock(graph.Mutex());
		--node->_holds;
		graph.MaybeComplete(*node);
		graph._impl->changed.notify_all();
	}

	// ---- Node ----

	Node::Node(Graph& a_graph, std::string a_name, NodeOptions a_options) :
		_graph(a_graph),
		_name(std::move(a_name)),
		_options(a_options)
	{
		_options.priority = (std::max)(_options.priority, 1u);
	}

	Node& Node::After(Node& a_other)
	{
		std::scoped_lock lock(_graph.Mutex());
		if (!a_other._complete) {
			a_other._dependents.push_back(this);
			++_waitingOn;
		}
		return *this;
	}

	Node& Node::Feeds(Node& a_other)
	{
		std::scoped_lock lock(_graph.Mutex());
		a_other._fed = true;
		if (!_complete) {
			_feeds.push_back(&a_other);
			++a_other._openFeeders;
		} else if (a_other._openFeeders == 0) {
			a_other._closed = true;
			_graph.MaybeComplete(a_other);
		}
		return *this;
	}

	void Node::Close()
	{
		std::scoped_lock lock(_graph.Mutex());
		_closed = true;
		_graph.MaybeComplete(*this);
	}

	Hold Node::Holding()
	{
		std::scoped_lock lock(_graph.Mutex());
		++_holds;
		return Hold(this);
	}

	bool Node::Complete() const
	{
		std::scoped_lock lock(_graph.Mutex());
		return _complete;
	}

	void Node::Pushed(std::size_t a_count)
	{
		if (_closed && _holds == 0) throw std::logic_error("jobs: items pushed to '" + _name + "' after it closed");
		_pushed += a_count;
		_graph.Wake();
	}

	// ---- Graph ----

	Graph::Graph(GraphOptions a_options) :
		_impl(std::make_unique<Impl>())
	{
		_impl->options = std::move(a_options);
	}

	Graph::~Graph()
	{
		{
			std::scoped_lock lock(_impl->mutex);
			_impl->ioStopping = true;
		}
		_impl->ioWork.notify_all();
		for (auto& thread : _impl->io) {
			if (thread.joinable()) thread.join();
		}
		// The nodes' state, a stage's models and images, at once across the cores rather than one node after another.
		TW_ZONE("jobs graph release");
		auto& nodes = _impl->nodes;
		oneapi::tbb::parallel_for(std::size_t{ 0 }, nodes.size(), std::size_t{ 1 }, [&](std::size_t i) { nodes[i]->Release(); });
	}

	std::mutex& Graph::Mutex() { return _impl->mutex; }

	void Graph::Adopt(std::unique_ptr<Node> a_node, const std::vector<Node*>& a_after)
	{
		std::scoped_lock lock(_impl->mutex);
		a_node->_index = _impl->nodes.size();
		a_node->_pass = _impl->virtualTime;
		for (auto* other : a_after) {
			if (other->_complete) continue;
			other->_dependents.push_back(a_node.get());
			++a_node->_waitingOn;
		}
		++_impl->incomplete;
		_impl->nodes.push_back(std::move(a_node));
	}

	Node& Graph::MakeJob(std::string a_name, std::function<void(JobContext&)> a_work, NodeOptions a_options, std::vector<Node*> a_after)
	{
		auto& queue = MakeQueue<int>(std::move(a_name), [work = std::move(a_work)](int&, JobContext& a_context) { work(a_context); }, a_options,
			std::move(a_after));
		queue.Push(0);
		queue.Close();
		return queue;
	}

	void Graph::Runnable(Node& a_node)
	{
		a_node._pass = (std::max)(a_node._pass, _impl->virtualTime);
		MaybeComplete(a_node);
		Wake();
	}

	void Graph::MaybeComplete(Node& a_node)
	{
		if (a_node._complete || !a_node._closed || a_node._waitingOn != 0 || !a_node.Empty() || a_node._active != 0 || a_node._holds != 0) return;
		a_node._complete = true;
		--_impl->incomplete;
		for (auto* dependent : a_node._dependents) {
			if (--dependent->_waitingOn == 0) Runnable(*dependent);
		}
		for (auto* fed : a_node._feeds) {
			if (--fed->_openFeeders == 0) {
				fed->_closed = true;
				MaybeComplete(*fed);
			}
		}
		_impl->changed.notify_all();
	}

	bool Graph::Claimable(const Node& a_node, Lane a_lane)
	{
		const auto& options = a_node._options;
		return !a_node._complete && options.lane == a_lane && a_node._waitingOn == 0 && !a_node.Empty() &&
		       (options.maxParallel == 0 || a_node._active < options.maxParallel);
	}

	void Graph::Wake()
	{
		auto& impl = *_impl;
		if (!impl.running || impl.stopped) return;
		// Workers enough for the Cpu items that can run now, beside Run's own thread.
		std::size_t cpu = 0;
		bool io = false;
		for (const auto& node : impl.nodes) {
			if (Claimable(*node, Lane::Cpu)) {
				const auto room = node->_options.maxParallel == 0 ? (std::numeric_limits<std::size_t>::max)() : node->_options.maxParallel - node->_active;
				cpu += (std::min)(room, node->_pushed - node->_done - node->_active);
			}
			io = io || Claimable(*node, Lane::Io);
			if (cpu > impl.workers && io) break;
		}
		while (impl.drains < impl.workers && impl.drains < cpu) {
			++impl.drains;
			impl.arena->enqueue([this] { Drain(); });
		}
		if (cpu > 0) impl.changed.notify_all();
		if (io) impl.ioWork.notify_all();
	}

	Graph::Claim Graph::TryClaim(Lane a_lane)
	{
		auto& impl = *_impl;
		if (impl.stopped) return {};
		if (impl.options.cancelled && impl.options.cancelled()) {
			impl.stopped = true;
			impl.changed.notify_all();
			impl.ioWork.notify_all();
			return {};
		}
		Node* best = nullptr;
		for (const auto& node : impl.nodes) {
			if (!Claimable(*node, a_lane)) continue;
			if (!best || node->_pass < best->_pass) best = node.get();
		}
		if (!best) return {};
		impl.virtualTime = best->_pass;
		best->_pass += 1.0 / best->_options.priority;
		++best->_active;
		++impl.active;
		return { best, best->Take() };
	}

	void Graph::Execute(Claim a_claim)
	{
		auto& impl = *_impl;
		JobContext context(*this, *a_claim.node);
		try {
			TW_ZONE_DYNAMIC(a_claim.node->_name);
			oneapi::tbb::this_task_arena::isolate([&] { a_claim.work(context); });
		} catch (...) {
			std::scoped_lock lock(impl.mutex);
			if (!impl.error) impl.error = std::current_exception();
			impl.stopped = true;
			impl.ioWork.notify_all();
		}
		a_claim.work = nullptr;
		std::optional<double> progress;
		{
			std::scoped_lock lock(impl.mutex);
			auto& node = *a_claim.node;
			--node._active;
			++node._done;
			--impl.active;
			MaybeComplete(node);
			Wake();
			if (impl.active == 0) impl.changed.notify_all();
			if (impl.options.progress && Clock::now() - impl.lastProgress >= kProgressEvery) {
				impl.lastProgress = Clock::now();
				progress = ProgressLocked();
			}
		}
		if (progress) impl.options.progress(*progress, a_claim.node->_name);
	}

	// One item, then back to TBB: a thread between items takes the tasks of parallel loops running inside other items before
	// another item, so those loops aren't left to the thread that started them.
	void Graph::Drain()
	{
		auto& impl = *_impl;
		Claim claim;
		{
			std::scoped_lock lock(impl.mutex);
			claim = TryClaim(Lane::Cpu);
			if (!claim.node) {
				--impl.drains;
				impl.changed.notify_all();
				return;
			}
		}
		Execute(std::move(claim));
		std::scoped_lock lock(impl.mutex);
		--impl.drains;
		Wake();
		impl.changed.notify_all();
	}

	void Graph::IoThread()
	{
		auto& impl = *_impl;
		for (;;) {
			Claim claim;
			{
				std::unique_lock lock(impl.mutex);
				impl.ioWork.wait(lock, [&] {
					if (impl.ioStopping) return true;
					claim = TryClaim(Lane::Io);
					return claim.node != nullptr;
				});
				if (!claim.node) return;
			}
			Execute(std::move(claim));
		}
	}

	std::string Graph::Stuck() const
	{
		std::string names;
		for (const auto& node : _impl->nodes) {
			if (node->_complete) continue;
			names += (names.empty() ? "" : ", ") + node->_name;
			if (!node->_closed) names += " (never closed)";
			else if (node->_waitingOn != 0) names += " (waiting on " + std::to_string(node->_waitingOn) + ")";
		}
		return names;
	}

	void Graph::Run()
	{
		auto& impl = *_impl;
		{
			std::scoped_lock lock(impl.mutex);
			if (impl.running) throw std::logic_error("jobs: a graph runs once");
			const auto threads = impl.options.threads != 0 ? static_cast<int>(impl.options.threads) : oneapi::tbb::info::default_concurrency();
			// One slot for Run's own thread, the rest for TBB's workers.
			impl.arena.emplace(threads, 1);
			impl.workers = static_cast<std::uint32_t>((std::max)(threads - 1, 0));
			impl.running = true;
			for (std::uint32_t i = 0; i < impl.options.ioThreads; ++i) impl.io.emplace_back([this] { IoThread(); });
		}
		impl.arena->execute([&] {
			std::unique_lock lock(impl.mutex);
			Wake();
			for (;;) {
				if (impl.stopped ? impl.active == 0 : impl.incomplete == 0) break;
				if (auto claim = TryClaim(Lane::Cpu); claim.node) {
					lock.unlock();
					Execute(std::move(claim));
					lock.lock();
					continue;
				}
				// Nothing to run, nothing running, nothing held, nothing to come: what's left can never complete.
				if (impl.active == 0 && impl.drains == 0 && !impl.stopped &&
					std::ranges::none_of(impl.nodes, [](const auto& a_node) { return !a_node->_complete && (a_node->_holds != 0 || !a_node->Empty()); })) {
					impl.error = std::make_exception_ptr(std::logic_error("jobs: nodes that can never complete: " + Stuck()));
					impl.stopped = true;
					break;
				}
				// Woken by work, the end, or in a while to see whether the run was cancelled.
				impl.changed.wait_for(lock, std::chrono::milliseconds(20));
				if (!impl.stopped && impl.options.cancelled && impl.options.cancelled()) impl.stopped = true;
			}
			// The workers' drains end once they find nothing to claim.
			impl.changed.wait(lock, [&] { return impl.drains == 0 && impl.active == 0; });
			impl.ioStopping = true;
		});
		impl.ioWork.notify_all();
		for (auto& thread : impl.io) thread.join();
		impl.io.clear();
		if (impl.options.progress) impl.options.progress(Progress(), "done");
		if (impl.error) std::rethrow_exception(impl.error);
	}

	void Graph::Cancel()
	{
		std::scoped_lock lock(_impl->mutex);
		_impl->stopped = true;
		_impl->changed.notify_all();
		_impl->ioWork.notify_all();
	}

	bool Graph::Cancelled() const
	{
		if (_impl->stopped) return true;
		return _impl->options.cancelled && _impl->options.cancelled();
	}

	double Graph::Progress() const
	{
		std::scoped_lock lock(_impl->mutex);
		return ProgressLocked();
	}

	double Graph::ProgressLocked() const
	{
		double total = 0.0, done = 0.0;
		for (const auto& node : _impl->nodes) {
			total += node->_options.weight;
			if (node->_complete) {
				done += node->_options.weight;
			} else if (node->_pushed > 0) {
				done += node->_options.weight * static_cast<double>(node->_done) / static_cast<double>(node->_pushed);
			}
		}
		return total > 0.0 ? done / total : 1.0;
	}
}