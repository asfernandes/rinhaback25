#pragma once

#include <string>
#include <cstdlib>


namespace rinhaback::proxy
{
	class Config final
	{
	private:
		static std::string readEnv(const char* name, const char* defaultVal)
		{
			const auto val = std::getenv(name);
			return val ? val : defaultVal;
		}

	public:
		Config() = delete;

	public:
		static inline const auto ioWorkers = static_cast<unsigned>(std::stoul(readEnv("IO_WORKERS", "8")));
		static inline const auto listenAddress = readEnv("LISTEN_ADDRESS", "0.0.0.0:9999");
		static inline const auto backend0Address = readEnv("BACKEND_0_ADDRESS", "localhost:8001");
		static inline const auto backend1Address = readEnv("BACKEND_1_ADDRESS", "localhost:8002");
	};
}  // namespace rinhaback::proxy
