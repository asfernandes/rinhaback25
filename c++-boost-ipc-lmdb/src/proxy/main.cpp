#include "mimalloc-new-delete.h"
#include "./Config.h"
#include "../common/Protocol.h"
#include "../common/Util.h"
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include "boost/asio.hpp"
#include "boost/beast.hpp"
#include "boost/json.hpp"
#include "boost/url.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
using tcp = asio::ip::tcp;

namespace
{
	using namespace rinhaback;
	using namespace rinhaback::proxy;

	constexpr std::chrono::seconds connectionTimeout{30};
	std::unique_ptr<asio::io_context> ioc;
	std::unique_ptr<IpcConnection> ipcConnection;
	std::atomic_uint backendIds;
	thread_local std::optional<unsigned> backendIdOpt;

	class Session final : public std::enable_shared_from_this<Session>
	{
	public:
		explicit Session(tcp::socket socket)
			: frontendStream(std::move(socket))
		{
			frontendStream.expires_after(connectionTimeout);
		}

		asio::awaitable<void> start()
		{
			boost::system::error_code ec;
			co_await http::async_read(frontendStream, buffer, request, asio::redirect_error(asio::use_awaitable, ec));

			if (ec)
			{
				if (ec != beast::http::error::end_of_stream && ec != asio::error::operation_aborted)
				{
					std::println(stderr, "Read request error: {}", ec.message());
					std::fflush(stderr);
				}

				co_return;
			}

			co_await processRequest();
		}

	private:
		asio::awaitable<void> processRequest()
		{
			if (!backendIdOpt.has_value())
				backendIdOpt = backendIds++;

			const auto backendId = backendIdOpt.value();

			auto& message = ipcConnection->header->items[backendId];

			response.result(http::status::not_found);

			switch (request.method())
			{
				case http::verb::get:
				{
					const auto url = boost::urls::parse_origin_form(request.target()).value();

					if (url.path() == "/payments-summary")
					{
						message.messageType = IpcMessageType::REQUEST_PAYMENTS_SUMMARY;
						message.paymentsSummaryRequest = {};

						const auto urlParams = url.params();

						if (const auto fromParam = urlParams.find("from"); fromParam != urlParams.end())
							message.paymentsSummaryRequest.from = parseDateTime((*fromParam).value);

						if (const auto toParam = urlParams.find("to"); toParam != urlParams.end())
							message.paymentsSummaryRequest.to = parseDateTime((*toParam).value);

						message.requestReady.post();
						message.responseReady.wait();

						const auto& defaultGateway = message.paymentsSummaryResponse.defaultGateway;
						const auto& fallbackGateway = message.paymentsSummaryResponse.fallbackGateway;

						response.result(http::status::ok);
						response.set(http::field::content_type, "application/json");

						response.body() = std::format(R"({{"default":{{"totalRequests":{},"totalAmount":{:.2f}}},)"
													  R"("fallback":{{"totalRequests":{},"totalAmount":{:.2f}}}}})",
							defaultGateway.totalRequests, defaultGateway.totalAmount, fallbackGateway.totalRequests,
							fallbackGateway.totalAmount);
					}
					break;
				}

				case http::verb::post:
					if (request.target() == "/payments")
					{
						message.postPaymentRequest = {};

						auto inJsonObj = boost::json::parse(request.body()).as_object();
						const auto& correlationIdJson = inJsonObj["correlationId"];
						const auto amountJson = inJsonObj["amount"];

						if (correlationIdJson.is_string() && amountJson.is_number())
						{
							const auto& correlationId = correlationIdJson.as_string();
							message.postPaymentRequest.amount = amountJson.to_number<double>();

							if (correlationId.size() == std::tuple_size<CorrelationId>() &&
								message.postPaymentRequest.amount > 0)
							{
								std::copy_n(correlationId.data(), message.postPaymentRequest.correlationId.size(),
									message.postPaymentRequest.correlationId.begin());

								message.messageType = IpcMessageType::REQUEST_POST_PAYMENT;

								message.requestReady.post();

								response.result(http::status::ok);
							}
							else
								response.result(http::status::bad_request);
						}
					}
					else if (request.target() == "/purge-payments")
					{
						message.messageType = IpcMessageType::REQUEST_PURGE_PAYMENTS;

						message.requestReady.post();
						message.responseReady.wait();

						response.result(http::status::ok);
					}
					break;

				default:
					break;
			}

			response.result(http::status::ok);
			response.version(request.version());
			response.keep_alive(response.result() == http::status::ok);
			response.prepare_payload();

			boost::system::error_code ec;
			co_await http::async_write(frontendStream, response, asio::redirect_error(asio::use_awaitable, ec));

			if (ec && ec != asio::error::operation_aborted)
			{
				std::println(stderr, "Write response error: {}", ec.message());
				std::fflush(stderr);
			}

			boost::system::error_code shutdownEc;
			frontendStream.socket().shutdown(tcp::socket::shutdown_send, shutdownEc);
		}

	private:
		beast::tcp_stream frontendStream;
		beast::flat_buffer buffer;
		http::request<http::string_body> request;
		http::response<http::string_body> response;
	};

	class Server final : public std::enable_shared_from_this<Server>
	{
	public:
		explicit Server(const tcp::endpoint& listenEndpoint)
			: acceptor(*ioc, listenEndpoint)
		{
			acceptor.set_option(asio::socket_base::reuse_address(true));
		}

		Server(const Server&) = delete;
		Server& operator=(const Server&) = delete;

		asio::awaitable<void> start()
		{
			co_await acceptConnections();
		}

	private:
		asio::awaitable<void> acceptConnections()
		{
			while (true)
			{
				boost::system::error_code ec;
				auto socket = co_await acceptor.async_accept(asio::redirect_error(asio::use_awaitable, ec));

				if (ec)
				{
					if (ec != asio::error::operation_aborted)
					{
						std::println(stderr, "Accept error: {}", ec.message());
						std::fflush(stderr);
					}

					break;
				}

				auto session = std::make_shared<Session>(std::move(socket));

				asio::co_spawn(
					acceptor.get_executor(),
					[session]() -> asio::awaitable<void>
					{
						// Start session
						co_await session->start();
					},
					asio::detached);
			}
		}

	private:
		tcp::acceptor acceptor;
	};

	void run()
	{
		ipcConnection = std::make_unique<IpcConnection>(std::nullopt);

		ioc = std::make_unique<asio::io_context>(Config::workers);

		const auto [ip, port] = parseHostPort(Config::listenAddress, 8080);
		const auto endpoint = tcp::endpoint{asio::ip::make_address(ip), port};

		auto server = std::make_shared<Server>(endpoint);

		asio::co_spawn(
			*ioc,
			[server]() -> asio::awaitable<void>
			{
				// Start server
				co_await server->start();
			},
			asio::detached);

		std::println("Server listening on {}", Config::listenAddress);
		std::fflush(stdout);

		std::vector<std::jthread> threads;
		threads.reserve(Config::workers - 1);

		for (unsigned i = 1; i < Config::workers; ++i)
		{
			threads.emplace_back(
				[]
				{
					try
					{
						ioc->run();
					}
					catch (const std::exception& e)
					{
						std::println(stderr, "Worker thread error: {}", e.what());
						std::fflush(stderr);
					}
				});
		}

		ioc->run();

		std::println("Proxy stopped");
		std::fflush(stdout);
	}
}  // namespace

int main(int argc, char* argv[])
{
	try
	{
		run();
		return 0;
	}
	catch (const std::exception& e)
	{
		std::println(stderr, "Fatal error: {}", e.what());
		std::fflush(stderr);
		return 1;
	}
}
