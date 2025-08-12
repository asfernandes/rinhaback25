#pragma once

#include <string>
#include <cstdlib>


namespace rinhaback::api
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
		static inline const auto handlerWorkers = static_cast<unsigned>(std::stoul(readEnv("HANDLER_WORKERS", "8")));
		static inline const auto database = readEnv("DATABASE", "/data/database");
		static inline const auto databaseSize = static_cast<unsigned>(std::stoul(readEnv("DATABASE_SIZE", "10485760")));
		static inline const auto coordinator = readEnv("COORDINATOR", "false") == "true";
		static inline const auto listenAddress = readEnv("LISTEN_ADDRESS", "0.0.0.0:8080");
		static inline const auto processorDefaultAddress =
			readEnv("PROCESSOR_DEFAULT_ADDRESS", "payment-processor-default:8080");
		static inline const auto processorFallbackAddress =
			readEnv("PROCESSOR_FALLBACK_ADDRESS", "payment-processor-fallback:8080");
	};
}  // namespace rinhaback::api
