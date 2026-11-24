#pragma once

#include <Render/Runtime/ITaskService.h>

#include <memory>

namespace tesworld::gpu
{
	// The task service tesworld's ORG runtime is given (ORG requires the host to bring one).
	//
	// - Submit() runs work on a fixed pool, highest priority first.
	// - ScheduleAfter() runs work once its delay has elapsed.
	// - ParallelFor*() fan out across the pool; the calling thread participates, so a
	//   worker may call them re-entrantly without deadlocking.
	// - A scope tracks the tasks submitted through it: Cancel() drops those not yet
	//   started, Wait() blocks until those started have finished.
	class ThreadPoolTaskService final : public org::runtime::ITaskService
	{
	public:
		// workerCount == 0 picks hardware_concurrency - 1 (at least one).
		explicit ThreadPoolTaskService(size_t workerCount = 0);
		~ThreadPoolTaskService() override;
		ThreadPoolTaskService(const ThreadPoolTaskService&) = delete;
		ThreadPoolTaskService& operator=(const ThreadPoolTaskService&) = delete;

		void ParallelFor(std::string_view taskName, size_t itemCount, std::function<void(size_t)> func) override;
		void ParallelForLimited(std::string_view taskName, size_t itemCount, size_t maximumConcurrency,
			std::function<void(size_t)> func) override;
		std::shared_ptr<org::runtime::ITaskScope> CreateScope(std::string_view name) override;
		bool Submit(const std::shared_ptr<org::runtime::ITaskScope>& scope, org::runtime::TaskPriority priority,
			std::string_view taskName, std::function<void()>&& func) override;
		bool ScheduleAfter(const std::shared_ptr<org::runtime::ITaskScope>& scope, std::chrono::steady_clock::duration delay,
			org::runtime::TaskPriority priority, std::string_view taskName, std::function<void()>&& func) override;

		size_t WorkerCount() const noexcept;

	private:
		struct Impl;
		std::shared_ptr<Impl> m_impl;
	};
}
