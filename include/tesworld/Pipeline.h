#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <oneapi/tbb/parallel_pipeline.h>
#include <oneapi/tbb/task_arena.h>

// The part of SARP's world streaming that is not its renderer: work items pass through typed stages, each serial or
// parallel, with a cap on items in flight and a byte budget on what they hold, cancellation, and progress. Assets
// shared between items load once through a byte-bounded cache.
namespace tesworld::pipeline
{
	class CancellationToken
	{
	public:
		void Cancel() noexcept { _cancelled.store(true, std::memory_order_release); }
		[[nodiscard]] bool IsCancelled() const noexcept { return _cancelled.load(std::memory_order_acquire); }

	private:
		std::atomic_bool _cancelled{ false };
	};

	// Bytes admitted into a pipeline. An item that alone exceeds the budget is admitted once nothing else is in flight,
	// so every item runs.
	class ByteBudget
	{
	public:
		explicit ByteBudget(std::size_t a_bytes) :
			_limit(a_bytes)
		{}

		void Acquire(std::size_t a_bytes)
		{
			std::unique_lock lock(_mutex);
			_released.wait(lock, [&] { return _inFlight == 0 || _used + a_bytes <= _limit; });
			_used += a_bytes;
			++_inFlight;
		}

		void Release(std::size_t a_bytes)
		{
			{
				std::scoped_lock lock(_mutex);
				_used -= (std::min)(_used, a_bytes);
				--_inFlight;
			}
			_released.notify_all();
		}

		[[nodiscard]] std::size_t Used() const
		{
			std::scoped_lock lock(_mutex);
			return _used;
		}

	private:
		mutable std::mutex _mutex;
		std::condition_variable _released;
		std::size_t _limit;
		std::size_t _used{ 0 };
		std::size_t _inFlight{ 0 };
	};

	struct RunOptions
	{
		// Items in flight at once across all stages (TBB's live tokens).
		std::size_t maxInFlight{ 0 };
		std::size_t byteBudget{ static_cast<std::size_t>(-1) };
		const CancellationToken* cancel{ nullptr };
		// Called after each item leaves the last stage, from that stage's thread.
		std::function<void(std::size_t a_done, std::size_t a_total)> progress;
	};

	// Items flow through the stages in order. A serial stage sees one item at a time (out of order); a parallel stage
	// any number. Exceptions from a stage cancel the run and are rethrown from Run.
	template <class Item>
	class Pipeline
	{
	public:
		using CostFunction = std::function<std::size_t(const Item&)>;

		Pipeline& Stage(std::string a_name, bool a_serial, std::function<void(Item&)> a_work)
		{
			_stages.push_back(StageInfo{ std::move(a_name), a_serial, std::move(a_work) });
			return *this;
		}

		Pipeline& Cost(CostFunction a_cost)
		{
			_cost = std::move(a_cost);
			return *this;
		}

		void Run(std::vector<Item>& a_items, const RunOptions& a_options) const
		{
			if (a_items.empty() || _stages.empty()) {
				return;
			}
			const auto concurrency = static_cast<std::size_t>(oneapi::tbb::this_task_arena::max_concurrency());
			const auto tokens = a_options.maxInFlight != 0 ? a_options.maxInFlight : concurrency * 2;
			ByteBudget budget(a_options.byteBudget);
			std::atomic_size_t next{ 0 };
			std::atomic_size_t done{ 0 };
			std::exception_ptr failure;
			std::mutex failureMutex;
			const auto cancelled = [&] {
				std::scoped_lock lock(failureMutex);
				return failure != nullptr || (a_options.cancel && a_options.cancel->IsCancelled());
			};

			struct Token
			{
				Item* item{ nullptr };
				std::size_t cost{ 0 };
			};

			auto filters = oneapi::tbb::make_filter<void, Token>(
				oneapi::tbb::filter_mode::serial_in_order,
				[&](oneapi::tbb::flow_control& a_control) -> Token {
					const auto index = next.fetch_add(1);
					if (index >= a_items.size() || cancelled()) {
						a_control.stop();
						return {};
					}
					Token token{ &a_items[index], _cost ? _cost(a_items[index]) : 0 };
					budget.Acquire(token.cost);
					return token;
				});

			for (std::size_t i = 0; i < _stages.size(); ++i) {
				const auto& stage = _stages[i];
				const bool last = i + 1 == _stages.size();
				auto filter = oneapi::tbb::make_filter<Token, Token>(
					stage.serial ? oneapi::tbb::filter_mode::serial_out_of_order : oneapi::tbb::filter_mode::parallel,
					[&, last](Token a_token) -> Token {
						if (a_token.item && !cancelled()) {
							try {
								stage.work(*a_token.item);
							} catch (...) {
								std::scoped_lock lock(failureMutex);
								if (!failure) failure = std::current_exception();
							}
						}
						if (last && a_token.item) {
							budget.Release(a_token.cost);
							const auto count = done.fetch_add(1) + 1;
							if (a_options.progress) a_options.progress(count, a_items.size());
						}
						return a_token;
					});
				filters = filters & filter;
			}

			auto sink = oneapi::tbb::make_filter<Token, void>(oneapi::tbb::filter_mode::parallel, [](Token) {});
			oneapi::tbb::parallel_pipeline(tokens, filters & sink);
			if (failure) {
				std::rethrow_exception(failure);
			}
		}

	private:
		struct StageInfo
		{
			std::string name;
			bool serial{ false };
			std::function<void(Item&)> work;
		};

		std::vector<StageInfo> _stages;
		CostFunction _cost;
	};

	// Values loaded once per key and shared: concurrent requests for the same key wait for one load. Keeps the most
	// recently used values while their sizes fit the budget; a value still held by a caller stays alive regardless.
	template <class Key, class Value, class Hash = std::hash<Key>>
	class AssetCache
	{
	public:
		using Pointer = std::shared_ptr<const Value>;
		// Loads a value and reports its size in bytes; returns null when there is nothing to load.
		using Loader = std::function<Pointer(const Key&, std::size_t& a_bytes)>;

		explicit AssetCache(std::size_t a_byteBudget) :
			_budget(a_byteBudget)
		{}

		Pointer GetOrLoad(const Key& a_key, const Loader& a_loader)
		{
			std::shared_future<Pointer> pending;
			std::promise<Pointer> promise;
			bool loadHere = false;
			{
				std::scoped_lock lock(_mutex);
				if (const auto it = _entries.find(a_key); it != _entries.end()) {
					_lru.splice(_lru.begin(), _lru, it->second.position);
					++_hits;
					return it->second.value;
				}
				if (const auto it = _loading.find(a_key); it != _loading.end()) {
					pending = it->second;
				} else {
					pending = promise.get_future().share();
					_loading.emplace(a_key, pending);
					loadHere = true;
				}
			}
			if (!loadHere) {
				return pending.get();
			}

			std::size_t bytes = 0;
			Pointer value;
			try {
				value = a_loader(a_key, bytes);
			} catch (...) {
				value = nullptr;
			}
			{
				std::scoped_lock lock(_mutex);
				_loading.erase(a_key);
				++_misses;
				_lru.push_front(a_key);
				_entries.emplace(a_key, Entry{ value, bytes, _lru.begin() });
				_used += bytes;
				Trim();
			}
			promise.set_value(value);
			return value;
		}

		[[nodiscard]] std::size_t Hits() const { std::scoped_lock lock(_mutex); return _hits; }
		[[nodiscard]] std::size_t Misses() const { std::scoped_lock lock(_mutex); return _misses; }
		[[nodiscard]] std::size_t BytesUsed() const { std::scoped_lock lock(_mutex); return _used; }

	private:
		struct Entry
		{
			Pointer value;
			std::size_t bytes{ 0 };
			typename std::list<Key>::iterator position;
		};

		void Trim()
		{
			while (_used > _budget && _lru.size() > 1) {
				const auto key = _lru.back();
				_lru.pop_back();
				const auto it = _entries.find(key);
				_used -= it->second.bytes;
				_entries.erase(it);
			}
		}

		mutable std::mutex _mutex;
		std::size_t _budget;
		std::size_t _used{ 0 };
		std::size_t _hits{ 0 };
		std::size_t _misses{ 0 };
		std::list<Key> _lru;
		std::unordered_map<Key, Entry, Hash> _entries;
		std::unordered_map<Key, std::shared_future<Pointer>, Hash> _loading;
	};
}
