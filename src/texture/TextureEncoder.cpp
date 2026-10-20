#include "tesworld/TextureEncoder.h"

#include "tesworld/Profiling.h"

#include <oneapi/tbb/task_group.h>

#include <memory>

namespace tesworld::texture
{
	namespace
	{
		class CpuTextureEncoder final : public TextureEncoder
		{
		public:
			~CpuTextureEncoder() noexcept override { _tasks.wait(); }

			void Encode(EncodeRequest a_request, Callback a_done) override
			{
				// task_group runs const callables: the request is shared, not moved into the task.
				auto request = std::make_shared<EncodeRequest>(std::move(a_request));
				_tasks.run([request, done = std::move(a_done)] {
					TW_ZONE("CPU texture encode");
					EncodeResult result;
					std::string error;
					auto dds = EncodeDds(request->chain, request->format, request->options, &error);
					// The chain's memory goes now, not when the task does.
					request->chain = {};
					if (dds) {
						result.dds = std::move(*dds);
					} else {
						result.error = error.empty() ? std::string("encoding failed") : error;
					}
					done(std::move(result));
				});
			}

			void Wait() override
			{
				TW_ZONE("CPU texture encode wait");
				_tasks.wait();
			}

			[[nodiscard]] std::string Backend() const override { return "CPU"; }

		private:
			oneapi::tbb::task_group _tasks;
		};
	}

	std::unique_ptr<TextureEncoder> CreateCpuTextureEncoder()
	{
		return std::make_unique<CpuTextureEncoder>();
	}
}