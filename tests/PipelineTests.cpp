// The tests check with assert, so it stays on in every configuration.
#undef NDEBUG

#include "tesworld/Pipeline.h"

#include <atomic>
#include <cassert>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <thread>

using namespace tesworld::pipeline;

namespace
{
	struct Work
	{
		int input{ 0 };
		int doubled{ 0 };
		int written{ 0 };
	};

	void StagesRunInOrderWithSerialCap()
	{
		std::vector<Work> items(200);
		for (int i = 0; i < 200; ++i) items[i].input = i;

		std::atomic_int insideSerial{ 0 };
		std::atomic_int maxInsideSerial{ 0 };
		std::size_t lastProgress = 0;
		Pipeline<Work> pipeline;
		pipeline
			.Stage("double", false, [](Work& a_work) { a_work.doubled = a_work.input * 2; })
			.Stage("write", true, [&](Work& a_work) {
				const auto now = ++insideSerial;
				maxInsideSerial = (std::max)(maxInsideSerial.load(), now);
				a_work.written = a_work.doubled + 1;
				--insideSerial;
			});
		pipeline.Run(items, RunOptions{ .maxInFlight = 8, .progress = [&](std::size_t a_done, std::size_t) { lastProgress = a_done; } });
		for (int i = 0; i < 200; ++i) {
			assert(items[i].written == i * 2 + 1);
		}
		assert(maxInsideSerial == 1);
		assert(lastProgress == 200);
	}

	void BudgetBoundsBytesInFlight()
	{
		std::vector<Work> items(64);
		std::atomic_size_t held{ 0 };
		std::atomic_size_t peak{ 0 };
		Pipeline<Work> pipeline;
		pipeline.Cost([](const Work&) { return std::size_t{ 100 }; })
			.Stage("hold", false, [&](Work&) {
				const auto now = (held += 100);
				peak = (std::max)(peak.load(), now);
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				held -= 100;
			});
		pipeline.Run(items, RunOptions{ .maxInFlight = 32, .byteBudget = 300 });
		assert(peak <= 300);

		// An item larger than the whole budget still runs, alone.
		std::vector<Work> big(3);
		int ran = 0;
		Pipeline<Work> oversized;
		oversized.Cost([](const Work&) { return std::size_t{ 1000 }; }).Stage("run", true, [&](Work&) { ++ran; });
		oversized.Run(big, RunOptions{ .byteBudget = 10 });
		assert(ran == 3);
	}

	void CancelAndFailureStopTheRun()
	{
		std::vector<Work> items(1000);
		CancellationToken cancel;
		std::atomic_int ran{ 0 };
		Pipeline<Work> pipeline;
		pipeline.Stage("cancel", true, [&](Work&) {
			if (++ran == 10) cancel.Cancel();
		});
		pipeline.Run(items, RunOptions{ .maxInFlight = 2, .cancel = &cancel });
		assert(ran < 1000);

		Pipeline<Work> failing;
		failing.Stage("throw", false, [](Work& a_work) {
			if (a_work.input == 0) throw std::runtime_error("boom");
		});
		bool threw = false;
		try {
			failing.Run(items, RunOptions{});
		} catch (const std::runtime_error&) {
			threw = true;
		}
		assert(threw);
	}

	void CacheLoadsOncePerKeyAndTrims()
	{
		AssetCache<int, std::string> cache(10);
		std::atomic_int loads{ 0 };
		const auto loader = [&](const int& a_key, std::size_t& a_bytes) {
			++loads;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			a_bytes = 4;
			return std::make_shared<const std::string>(std::to_string(a_key));
		};

		std::vector<std::thread> threads;
		for (int i = 0; i < 8; ++i) {
			threads.emplace_back([&] { assert(*cache.GetOrLoad(7, loader) == "7"); });
		}
		for (auto& thread : threads) thread.join();
		assert(loads == 1);

		for (int key = 0; key < 5; ++key) {
			(void)cache.GetOrLoad(key, loader);
		}
		assert(cache.BytesUsed() <= 10);
		assert(*cache.GetOrLoad(4, loader) == "4");
		const auto before = loads.load();
		(void)cache.GetOrLoad(4, loader);
		assert(loads == before);
		assert(cache.Hits() >= 2);

		// A loader with nothing to load caches the miss.
		const auto missing = cache.GetOrLoad(99, [](const int&, std::size_t&) { return AssetCache<int, std::string>::Pointer{}; });
		assert(!missing);
	}
}

int main()
{
	StagesRunInOrderWithSerialCap();
	BudgetBoundsBytesInFlight();
	CancelAndFailureStopTheRun();
	CacheLoadsOncePerKeyAndTrims();
	std::cout << "tesworld pipeline tests passed\n";
	return 0;
}
