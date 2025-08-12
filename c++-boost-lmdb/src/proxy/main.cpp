#include "mimalloc-new-delete.h"
#include "./Config.h"
#include "./Util.h"
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <print>
#include <string>
#include <thread>
#include <vector>
#include "boost/asio.hpp"
#include "boost/beast.hpp"
#include "boost/url.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
using tcp = asio::ip::tcp;


namespace
{
	using namespace rinhaback::proxy;

	struct Backend final
	{
		std::string address;
		tcp::endpoint endpoint;
	};

	constexpr bool ASYNC_POST_PAYMENT = true;
	constexpr std::chrono::seconds connectionTimeout{30};
	std::unique_ptr<asio::io_context> ioc;
	std::array<Backend, 2> backends;
	std::atomic<size_t> nextBackend{0};

	class Session final : public std::enable_shared_from_this<Session>
	{
	private:
		enum class HandlerType
		{
			ASYNC,
			PROXY
		};

	public:
		explicit Session(tcp::socket socket)
			: frontendStream(std::move(socket)),
			  backendStream(*ioc)
		{
			frontendStream.expires_after(connectionTimeout);
		}

		asio::awaitable<void> start()
		{
			co_await readRequest();
		}

	private:
		asio::awaitable<void> readRequest()
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

		asio::awaitable<void> processRequest()
		{
			handlerType = determineHandler();

			if (handlerType == HandlerType::ASYNC)
				co_await handlePostPayment();

			co_await proxyToBackend();
		}

		HandlerType determineHandler()
		{
			if constexpr (ASYNC_POST_PAYMENT)
			{
				if (request.method() == http::verb::post)
				{
					try
					{
						const auto url = boost::urls::parse_origin_form(request.target());

						if (url && url->path() == "/payments")
							return HandlerType::ASYNC;
					}
					catch (const std::exception& e)
					{
						std::println(stderr, "URL parse error: {}", e.what());
						std::fflush(stderr);
					}
				}
			}

			return HandlerType::PROXY;
		}

		asio::awaitable<void> handlePostPayment()
		{
			response.result(http::status::ok);
			response.version(request.version());
			response.keep_alive(request.keep_alive());
			response.prepare_payload();

			co_await writeResponse();
		}

		asio::awaitable<void> proxyToBackend()
		{
			size_t backendIndex = nextBackend.fetch_add(1) % backends.size();
			const auto& backend = backends[backendIndex];

			backendStream.expires_after(connectionTimeout);

			boost::system::error_code ec;
			co_await backendStream.async_connect(backend.endpoint, asio::redirect_error(asio::use_awaitable, ec));

			if (ec)
			{
				std::println(stderr, "Backend connect error ({}:{}): {}", backend.endpoint.address().to_string(),
					backend.endpoint.port(), ec.message());
				std::fflush(stderr);
				co_await sendErrorResponse(http::status::bad_gateway);
				co_return;
			}

			auto backendRequest = std::make_shared<http::request<http::string_body>>(request);
			backendRequest->set(http::field::host, backend.address);
			backendRequest->prepare_payload();

			co_await http::async_write(backendStream, *backendRequest, asio::redirect_error(asio::use_awaitable, ec));

			if (ec)
			{
				std::println(stderr, "Backend write error: {}", ec.message());
				std::fflush(stderr);
				co_await sendErrorResponse(http::status::bad_gateway);
				co_return;
			}

			if (handlerType == HandlerType::ASYNC)
			{
				if (backendStream.socket().is_open())
				{
					boost::system::error_code shutdownEc;
					backendStream.socket().shutdown(tcp::socket::shutdown_both, shutdownEc);
				}
			}
			else
				co_await readBackendResponse();
		}

		asio::awaitable<void> readBackendResponse()
		{
			buffer.clear();

			boost::system::error_code ec;
			co_await http::async_read(backendStream, buffer, response, asio::redirect_error(asio::use_awaitable, ec));

			if (ec)
			{
				std::println(stderr, "Backend read error: {}", ec.message());
				std::fflush(stderr);
				co_await sendErrorResponse(http::status::bad_gateway);
				co_return;
			}

			co_await forwardResponseToClient();
		}

		asio::awaitable<void> forwardResponseToClient()
		{
			response.keep_alive(request.keep_alive());
			response.prepare_payload();
			co_await writeResponse();
		}

		asio::awaitable<void> sendErrorResponse(http::status status)
		{
			response.result(status);
			response.version(request.version());
			response.keep_alive(false);  // Close connection on error
			response.body() = "Proxy Error";
			response.prepare_payload();
			co_await writeResponse();
		}

		asio::awaitable<void> writeResponse()
		{
			boost::system::error_code ec;
			co_await http::async_write(frontendStream, response, asio::redirect_error(asio::use_awaitable, ec));

			if (ec && ec != asio::error::operation_aborted)
			{
				std::println(stderr, "Write response error: {}", ec.message());
				std::fflush(stderr);
			}

			boost::system::error_code shutdownEc;
			frontendStream.socket().shutdown(tcp::socket::shutdown_send, shutdownEc);

			if (handlerType != HandlerType::ASYNC)
			{
				if (backendStream.socket().is_open())
					backendStream.socket().shutdown(tcp::socket::shutdown_both, shutdownEc);
			}
		}

	private:
		HandlerType handlerType = HandlerType::PROXY;
		beast::tcp_stream frontendStream;
		beast::tcp_stream backendStream;
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
			acceptor.set_option(boost::asio::ip::tcp::no_delay(true));

			resolveBackends();
		}

		Server(const Server&) = delete;
		Server& operator=(const Server&) = delete;

		asio::awaitable<void> start()
		{
			co_await acceptConnections();
		}

	private:
		void resolveBackends()
		{
			tcp::resolver resolver{*ioc};

			backends[0].address = Config::backend0Address;
			backends[1].address = Config::backend1Address;

			try
			{
				for (auto& backend : backends)
				{
					const auto [host, port] = parseHostPort(backend.address, 8080);
					auto results = resolver.resolve(host, std::to_string(port));
					backend.endpoint = results.begin()->endpoint();
				}
			}
			catch (const std::exception& e)
			{
				std::println(stderr, "Failed to resolve backends: {}", e.what());
				std::fflush(stderr);
				throw;
			}
		}

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

				socket.set_option(boost::asio::ip::tcp::no_delay(true));

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
		ioc = std::make_unique<asio::io_context>(Config::ioWorkers);

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
		threads.reserve(Config::ioWorkers - 1);

		for (unsigned i = 1; i < Config::ioWorkers; ++i)
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
