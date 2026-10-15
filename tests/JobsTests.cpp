// The tests check with assert, so it stays on in every configuration.
#undef NDEBUG

#include "tesworld/Jobs.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace tesworld::jobs;

namespace
{
	void Spin(std::chrono::microseconds a_for)
	{
		const auto until = std::chrono::steady_clock::now() + a_for;
		while (std::chrono::steady_clock::now() < until) {
		}
	}

	// a feeds b and c, which both feed d: d sees every item of a's, through b and c, and runs after both.
	void DiamondAndFanOut()
	{
		Graph graph;
		std::atomic<int> b{ 0 }, c{ 0 }, d{ 0 };
		std::atomic<bool> bDone{ false }, cDone{ false };
		auto& dq = graph.MakeQueue<int>("d", [&](int& a_value, JobContext&) {
			assert(bDone && cDone);
			d += a_value;
		});
		auto& bq = graph.MakeQueue<int>("b", [&](int& a_value, JobContext&) {
			b += a_value;
		});
		auto& cq = graph.MakeQueue<int>("c", [&](int& a_value, JobContext&) {
			c += a_value;
		});
		auto& aq = graph.MakeQueue<int>("a", [&](int& a_value, JobContext&) {
			// Items push items: a fans out.
			bq.Push(a_value);
			cq.Push(a_value * 2);
		});
		aq.Feeds(bq);
		aq.Feeds(cq);
		auto& markB = graph.MakeJob("b done", [&](JobContext&) { bDone = true; }, {}, { &bq });
		auto& markC = graph.MakeJob("c done", [&](JobContext&) { cDone = true; }, {}, { &cq });
		dq.After(markB).After(markC);
		auto& feedD = graph.MakeJob("feed d", [&](JobContext&) { dq.Push(b + c); }, {}, { &markB, &markC });
		feedD.Feeds(dq);
		for (int i = 1; i <= 100; ++i) aq.Push(i);
		aq.Close();
		graph.Run();
		assert(b == 5050 && c == 10100 && d == 15150);
		assert(dq.Complete() && graph.Progress() == 1.0);
	}

	void ACapIsHonoured()
	{
		Graph graph;
		std::atomic<int> running{ 0 }, peak{ 0 };
		auto& queue = graph.MakeQueue<int>("capped", [&](int&, JobContext&) {
			const auto now = ++running;
			int seen = peak;
			while (now > seen && !peak.compare_exchange_weak(seen, now)) {
			}
			Spin(std::chrono::microseconds(500));
			--running;
		}, NodeOptions{ .maxParallel = 3 });
		for (int i = 0; i < 200; ++i) queue.Push(i);
		queue.Close();
		graph.Run();
		assert(peak <= 3 && peak >= 2);
	}

	// With one worker, claims go in proportion to priority: four of "high" to each of "low".
	void PriorityShares()
	{
		Graph graph(GraphOptions{ .threads = 1, .ioThreads = 0 });
		std::mutex mutex;
		std::string order;
		auto& high = graph.MakeQueue<int>("high", [&](int&, JobContext&) {
			std::scoped_lock lock(mutex);
			order += 'h';
		}, NodeOptions{ .priority = 4 });
		auto& low = graph.MakeQueue<int>("low", [&](int&, JobContext&) {
			std::scoped_lock lock(mutex);
			order += 'l';
		});
		for (int i = 0; i < 40; ++i) high.Push(i);
		for (int i = 0; i < 40; ++i) low.Push(i);
		high.Close();
		low.Close();
		graph.Run();
		const auto firstFifty = order.substr(0, 50);
		const auto highs = std::count(firstFifty.begin(), firstFifty.end(), 'h');
		assert(highs >= 38 && highs <= 42);
	}

	void CancellingStopsClaims()
	{
		std::atomic<bool> cancel{ false };
		Graph graph(GraphOptions{ .cancelled = [&] { return cancel.load(); } });
		std::atomic<int> ran{ 0 };
		auto& queue = graph.MakeQueue<int>("long", [&](int&, JobContext& a_context) {
			if (++ran == 10) cancel = true;
			Spin(std::chrono::microseconds(200));
			assert(ran < 10 || a_context.Cancelled());
		});
		for (int i = 0; i < 100000; ++i) queue.Push(i);
		queue.Close();
		graph.Run();
		assert(ran < 200 && graph.Cancelled());
	}

	void AnExceptionCancelsAndIsRethrown()
	{
		Graph graph;
		std::atomic<int> ran{ 0 };
		auto& queue = graph.MakeQueue<int>("throws", [&](int& a_value, JobContext&) {
			++ran;
			if (a_value == 5) throw std::runtime_error("five");
			Spin(std::chrono::microseconds(100));
		});
		for (int i = 0; i < 100000; ++i) queue.Push(i);
		queue.Close();
		bool caught = false;
		try {
			graph.Run();
		} catch (const std::runtime_error& e) {
			caught = std::string(e.what()) == "five";
		}
		assert(caught && ran < 100000);
	}

	// Io items block without holding up Cpu items, and a hold keeps a node open for work done elsewhere.
	void IoAndHolds()
	{
		Graph graph(GraphOptions{ .threads = 2, .ioThreads = 2 });
		std::atomic<int> writes{ 0 }, cpu{ 0 }, writing{ 0 }, peak{ 0 };
		auto& io = graph.MakeQueue<int>("write", [&](int&, JobContext&) {
			const auto now = ++writing;
			int seen = peak;
			while (now > seen && !peak.compare_exchange_weak(seen, now)) {
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			--writing;
			++writes;
		}, NodeOptions{ .lane = Lane::Io });
		auto& work = graph.MakeQueue<int>("work", [&](int& a_value, JobContext&) {
			++cpu;
			io.Push(a_value);
		});
		work.Feeds(io);
		for (int i = 0; i < 40; ++i) work.Push(i);
		work.Close();
		// Work done on a thread of its own pushes its result when it's ready.
		auto hold = io.Holding();
		std::thread elsewhere([&io, hold = std::move(hold)]() mutable {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			io.Push(1000);
			hold.Release();
		});
		graph.Run();
		elsewhere.join();
		// Both Io threads wrote at once.
		assert(cpu == 40 && writes == 41 && peak == 2);
	}

	void ANodeThatCanNeverCompleteIsNamed()
	{
		Graph graph;
		graph.MakeQueue<int>("forgotten", [](int&, JobContext&) {});
		bool caught = false;
		try {
			graph.Run();
		} catch (const std::logic_error& e) {
			caught = std::string(e.what()).find("forgotten (never closed)") != std::string::npos;
		}
		assert(caught);
	}

	// Many tiny items, pushed from items as well as up front, with nothing lost.
	void Stress()
	{
		Graph graph;
		std::atomic<std::uint64_t> sum{ 0 };
		auto& leaves = graph.MakeQueue<std::uint64_t>("leaves", [&](std::uint64_t& a_value, JobContext&) { sum += a_value; });
		auto& roots = graph.MakeQueue<std::uint64_t>("roots", [&](std::uint64_t& a_value, JobContext&) {
			for (std::uint64_t i = 0; i < 10; ++i) leaves.Push(a_value * 10 + i);
		});
		roots.Feeds(leaves);
		for (std::uint64_t i = 0; i < 10000; ++i) roots.Push(i);
		roots.Close();
		graph.Run();
		// 0 .. 99999.
		assert(sum == 99999ull * 100000ull / 2);
	}
}

int main()
{
	DiamondAndFanOut();
	ACapIsHonoured();
	PriorityShares();
	CancellingStopsClaims();
	AnExceptionCancelsAndIsRethrown();
	IoAndHolds();
	ANodeThatCanNeverCompleteIsNamed();
	Stress();
	std::cout << "tesworld jobs tests passed\n";
	return 0;
}