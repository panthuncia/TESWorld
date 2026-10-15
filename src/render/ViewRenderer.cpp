#include "tesworld/Render.h"
#include "tesworld/Profiling.h"

#include <oneapi/tbb/task_group.h>

#include <exception>
#include <memory>
#include <utility>

namespace tesworld::render
{
	namespace
	{
		class CpuViewRenderer final : public ViewRenderer
		{
		public:
			~CpuViewRenderer() noexcept override { _tasks.wait(); }

			void Render(ViewRequest a_request, Callback a_done) override
			{
				// task_group runs const callables: the request is shared, not moved into the task.
				auto request = std::make_shared<ViewRequest>(std::move(a_request));
				_tasks.run([request, done = std::move(a_done)] {
					TW_ZONE("CPU view render");
					ViewResult result;
					try {
						const TextureSource source = [&](const std::string& a_path) -> const texture::MipChain* {
							const auto it = request->textures.find(a_path);
							return it != request->textures.end() ? it->second.get() : nullptr;
						};
						const DrawItem item{ .model = request->model.get() };
						CpuRenderer renderer;
						result.frame = renderer.Render(std::span(&item, 1), request->camera, request->options, source);
					} catch (const std::exception& e) {
						result.error = e.what();
					}
					// What it drew with goes now, not when the task does.
					request->model.reset();
					request->textures.clear();
					done(std::move(result));
				});
			}

			void Wait() override
			{
				TW_ZONE("CPU view render wait");
				_tasks.wait();
			}

			[[nodiscard]] std::string Backend() const override { return "CPU"; }

		private:
			oneapi::tbb::task_group _tasks;
		};
	}

	std::unique_ptr<ViewRenderer> CreateCpuViewRenderer()
	{
		return std::make_unique<CpuViewRenderer>();
	}
}