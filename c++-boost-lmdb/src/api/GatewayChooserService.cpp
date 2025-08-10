#include "./GatewayChooserService.h"
#include "./Config.h"
#include "./SignalHandling.h"
#include "./Util.h"
#include <atomic>
#include <cstdio>
#include <optional>
#include <print>
#include <string_view>
#include <utility>
#include <cassert>
#include <experimental/scope>
#include "boost/interprocess/mapped_region.hpp"
#include "boost/interprocess/shared_memory_object.hpp"
#include "boost/interprocess/sync/interprocess_semaphore.hpp"
#include "boost/json.hpp"
#include "boost/asio.hpp"
#include "boost/beast.hpp"
#include "boost/url.hpp"
#include "boost/url/parse.hpp"

namespace boostipc = boost::interprocess;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
using tcp = asio::ip::tcp;


namespace
{
	using namespace rinhaback::api;

	struct SharedData
	{
		boostipc::interprocess_semaphore ready{0};
		std::atomic_uint8_t currentGateway{static_cast<std::uint8_t>(PaymentGateway::DEFAULT)};
	};

	class SharedMemoryManager
	{
	private:
		static constexpr const char* SHARED_MEMORY_NAME = "rinhaback25-boost-lmdb-GatewayChooserService";

	public:
		explicit SharedMemoryManager(bool isCreator = false)
		{
			if (isCreator)
			{
				boostipc::shared_memory_object::remove(SHARED_MEMORY_NAME);
				shm = boostipc::shared_memory_object(boostipc::create_only, SHARED_MEMORY_NAME, boostipc::read_write);

				shm.truncate(sizeof(SharedData));

				region = boostipc::mapped_region(shm, boostipc::read_write);
				data = new (region.get_address()) SharedData();

				std::println("GatewayChooserService initialized.");
				std::fflush(stdout);

				data->ready.post();
			}
			else
			{
				shm = boostipc::shared_memory_object(boostipc::open_only, SHARED_MEMORY_NAME, boostipc::read_write);
				region = boostipc::mapped_region(shm, boostipc::read_write);
				data = static_cast<SharedData*>(region.get_address());

				data->ready.wait();

				std::println("GatewayChooserService initialized by other process.");
				std::fflush(stdout);
			}
		}

	public:
		SharedData* data;

	private:
		boostipc::shared_memory_object shm;
		boostipc::mapped_region region;
	};

	struct GatewayHealthResponse
	{
		bool failing;
		int minResponseTime;
	};

	static SharedMemoryManager sharedMemoryManager{Config::coordinator};

	static std::optional<GatewayHealthResponse> getGatewayHealth(
		asio::io_context& ioc, const tcp::endpoint& endpoint, const std::string& host)
	{
		try
		{
			beast::tcp_stream stream{ioc};
			stream.connect(endpoint);

			http::request<http::string_body> req{http::verb::get, "/payments/service-health", 11};
			req.set(http::field::host, host);

			http::write(stream, req);

			beast::flat_buffer buffer;
			http::response<http::string_body> res;
			http::read(stream, buffer, res);

			boost::system::error_code shutdownEc;
			stream.socket().shutdown(tcp::socket::shutdown_both, shutdownEc);

			if (res.result() == http::status::ok)
			{
				auto jsonObj = boost::json::parse(res.body()).as_object();
				const auto failingJson = jsonObj["failing"];
				const auto minResponseTimeJson = jsonObj["minResponseTime"];

				if (failingJson.is_bool() && minResponseTimeJson.is_number())
				{
					return GatewayHealthResponse{
						.failing = failingJson.as_bool(),
						.minResponseTime = minResponseTimeJson.to_number<int>(),
					};
				}
			}
		}
		catch (const std::exception& e)
		{
			std::println(stderr, "Error getting gateway health: {}", e.what());
			std::fflush(stderr);
		}

		return std::nullopt;
	}
}  // namespace

namespace rinhaback::api
{
	std::jthread GatewayChooserService::start()
	{
		if (!Config::coordinator)
			return {};
		else
			return std::jthread(handler);
	}

	void GatewayChooserService::handler()
	{
		std::println("GatewayChooserService started.");
		std::fflush(stdout);

		asio::io_context ioc;
		tcp::resolver resolver{ioc};

		static tcp::endpoint defaultEndpoint, fallbackEndpoint;

		{  // scope
			const auto [host, port] = parseHostPort(Config::processorDefaultAddress, 8080);
			defaultEndpoint = resolver.resolve(host, std::to_string(port)).begin()->endpoint();
		}

		{  // scope
			const auto [host, port] = parseHostPort(Config::processorFallbackAddress, 8080);
			fallbackEndpoint = resolver.resolve(host, std::to_string(port)).begin()->endpoint();
		}

		auto lastDefaultCheck = std::chrono::steady_clock::now() - POLL_TIME;
		auto lastFallbackCheck = std::chrono::steady_clock::now() - POLL_TIME;

		std::optional<GatewayHealthResponse> defaultHealth;
		std::optional<GatewayHealthResponse> fallbackHealth;

		while (!SignalHandling::shouldFinish())
		{
			auto currentChoice = static_cast<PaymentGateway>(sharedMemoryManager.data->currentGateway.load());
			const auto now = std::chrono::steady_clock::now();

			if (now - lastDefaultCheck >= POLL_TIME)
			{
				if (const auto newDefaultHealth =
						getGatewayHealth(ioc, defaultEndpoint, Config::processorDefaultAddress))
				{
					defaultHealth = newDefaultHealth.value();
				}

				lastDefaultCheck = now;
			}

			if (now - lastFallbackCheck >= POLL_TIME)
			{
				if (const auto newFallbackHealth =
						getGatewayHealth(ioc, fallbackEndpoint, Config::processorFallbackAddress))
				{
					fallbackHealth = newFallbackHealth.value();
				}

				lastFallbackCheck = now;
			}

			auto newChoice = currentChoice;

			if (defaultHealth && fallbackHealth)
			{
				if (!defaultHealth->failing && !fallbackHealth->failing)
				{
					if (defaultHealth->minResponseTime > 100 &&
						defaultHealth->minResponseTime > fallbackHealth->minResponseTime * 2)
					{
						newChoice = PaymentGateway::FALLBACK;
					}
					else
						newChoice = PaymentGateway::DEFAULT;
				}
				else if (!defaultHealth->failing && fallbackHealth->failing)
					newChoice = PaymentGateway::DEFAULT;
				else if (defaultHealth->failing && !fallbackHealth->failing)
					newChoice = PaymentGateway::FALLBACK;
				else
					newChoice = PaymentGateway::DEFAULT;
			}
			else if (defaultHealth && !fallbackHealth)
			{
				if (!defaultHealth->failing)
					newChoice = PaymentGateway::DEFAULT;
				else
					newChoice = PaymentGateway::FALLBACK;
			}
			else if (!defaultHealth && fallbackHealth)
			{
				if (!fallbackHealth->failing)
				{
					newChoice =
						currentChoice == PaymentGateway::DEFAULT ? PaymentGateway::DEFAULT : PaymentGateway::FALLBACK;
				}
				else
					newChoice = PaymentGateway::DEFAULT;
			}
			else
				newChoice = PaymentGateway::DEFAULT;

			if (newChoice != currentChoice)
			{
				currentChoice = newChoice;
				sharedMemoryManager.data->currentGateway = static_cast<std::uint8_t>(currentChoice);

				std::println(
					"Gateway switched to: {}", currentChoice == PaymentGateway::DEFAULT ? "DEFAULT" : "FALLBACK");
				std::fflush(stdout);
			}

			if (defaultHealth.has_value())
			{
				std::println("DEFAULT health: failing: {}, minResponseTime: {}", defaultHealth->failing,
					defaultHealth->minResponseTime);
				std::fflush(stdout);
			}

			if (fallbackHealth.has_value())
			{
				std::println("FALLBACK health: failing: {}, minResponseTime: {}", fallbackHealth->failing,
					fallbackHealth->minResponseTime);
				std::fflush(stdout);
			}

			std::println("Current gateway: {}", currentChoice == PaymentGateway::DEFAULT ? "DEFAULT" : "FALLBACK");
			std::fflush(stdout);

			std::fflush(stdout);

			std::this_thread::sleep_for(POLL_TIME);
		}

		std::println("GatewayChooserService stopped.");
		std::fflush(stdout);
	}

	PaymentGateway GatewayChooserService::getGateway()
	{
		return static_cast<PaymentGateway>(sharedMemoryManager.data->currentGateway.load());
	}

	void GatewayChooserService::switchGatewayTo(PaymentGateway gateway)
	{
		sharedMemoryManager.data->currentGateway = std::to_underlying(gateway);
	}
}  // namespace rinhaback::api
