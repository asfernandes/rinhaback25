#include "mimalloc-new-delete.h"
#include "./PaymentProcessor.h"
#include "./Config.h"
#include "./GatewayChooserService.h"
#include "./PaymentService.h"
#include "./PendingPaymentsQueue.h"
#include "./SignalHandling.h"
#include "./Util.h"
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <thread>
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
	using namespace rinhaback::api;

	std::shared_ptr<PaymentService> paymentService{std::make_shared<PaymentService>()};
	std::shared_ptr<PendingPaymentsQueue> pendingPaymentsQueue{std::make_shared<PendingPaymentsQueue>()};
	std::unique_ptr<asio::io_context> ioc;
	std::unique_ptr<asio::thread_pool> workerPool;

	// Handler for GET /payments-summary
	void paymentsSummaryHandler(const boost::urls::url_view& url, http::response<http::string_body>& res)
	{
		std::optional<DateTimeMillis> from, to;

		const auto urlParams = url.params();

		if (const auto fromParam = urlParams.find("from"); fromParam != urlParams.end())
			from = parseDateTime((*fromParam).value);

		if (const auto toParam = urlParams.find("to"); toParam != urlParams.end())
			to = parseDateTime((*toParam).value);

		const auto summary = paymentService->getPaymentsSummary(from, to);

		const auto& defaultGateway = summary.defaultGateway;
		const auto& fallbackGateway = summary.fallbackGateway;

		res.body() = std::format(R"({{"default":{{"totalRequests":{},"totalAmount":{:.2f}}},)"
								 R"("fallback":{{"totalRequests":{},"totalAmount":{:.2f}}}}})",
			defaultGateway.totalRequests, defaultGateway.totalAmount, fallbackGateway.totalRequests,
			fallbackGateway.totalAmount);
		res.result(http::status::ok);
	}

	// Handler for POST /payments
	void postPaymentHandler(const boost::urls::url_view& url, http::request<http::string_body>& req)
	{
		auto inJsonObj = boost::json::parse(req.body()).as_object();
		const auto& correlationIdJson = inJsonObj["correlationId"];
		const auto amountJson = inJsonObj["amount"];

		if (correlationIdJson.is_string() && amountJson.is_number())
		{
			const auto& correlationId = correlationIdJson.as_string();
			const auto amount = amountJson.to_number<double>();

			if (correlationId.size() == std::tuple_size<CorrelationId>() && amount > 0)
			{
				PendingPaymentsQueue::Payment pendingPayment = {.amount = amount};
				std::copy_n(
					correlationId.data(), pendingPayment.correlationId.size(), pendingPayment.correlationId.begin());

				pendingPaymentsQueue->enqueue(pendingPayment);
			}
		}
	}

	// Handler for POST /purge-payments
	void purgePaymentsHandler(http::response<http::string_body>& res)
	{
		paymentService->purge();
		pendingPaymentsQueue->purge();

		res.result(http::status::ok);
	}

	asio::awaitable<void> sessionHandler(tcp::socket socket)
	{
		auto stream = std::make_shared<beast::tcp_stream>(std::move(socket));
		stream->expires_after(std::chrono::seconds(30));

		auto buffer = std::make_shared<beast::flat_buffer>();
		auto req = std::make_shared<http::request<http::string_body>>();

		boost::system::error_code ec;
		co_await http::async_read(*stream, *buffer, *req, asio::redirect_error(asio::use_awaitable, ec));

		if (ec)
		{
			std::println(stderr, "Read error: {}", ec.message());
			std::fflush(stderr);
			co_return;
		}

		auto res = std::make_shared<http::response<http::string_body>>(http::status::not_found, req->version());
		res->keep_alive(req->keep_alive());

		try
		{
			const auto url = boost::urls::parse_origin_form(req->target()).value();
			enum
			{
				HANDLER_PAYMENTS_SUMMARY,
				HANDLER_POST_PAYMENT,
				HANDLER_PURGE_PAYMENTS,
				HANDLER_ERROR
			} handler = HANDLER_ERROR;

			switch (req->method())
			{
				case http::verb::get:
					if (url.path() == "/payments-summary")
						handler = HANDLER_PAYMENTS_SUMMARY;
					break;

				case http::verb::post:
					if (url.path() == "/payments")
						handler = HANDLER_POST_PAYMENT;
					else if (url.path() == "/purge-payments")
						handler = HANDLER_PURGE_PAYMENTS;
					break;

				default:
					break;
			}

			if (handler == HANDLER_POST_PAYMENT)
			{
				res->result(http::status::ok);

				co_await http::async_write(*stream, *res, asio::redirect_error(asio::use_awaitable, ec));

				boost::system::error_code shutdownEc;
				stream->socket().shutdown(tcp::socket::shutdown_send, shutdownEc);
			}

			asio::post(*workerPool,
				[url, req, res, stream, handler]()
				{
					switch (handler)
					{
						case HANDLER_PAYMENTS_SUMMARY:
							paymentsSummaryHandler(url, *res);
							break;

						case HANDLER_POST_PAYMENT:
							postPaymentHandler(url, *req);
							break;

						case HANDLER_PURGE_PAYMENTS:
							purgePaymentsHandler(*res);
							break;

						default:
							break;
					}

					if (handler != HANDLER_POST_PAYMENT)
					{
						res->prepare_payload();

						asio::post(stream->get_executor(),
							[stream, res]()
							{
								asio::co_spawn(
									stream->get_executor(),
									[stream, res]() -> asio::awaitable<void>
									{
										boost::system::error_code ec;
										co_await http::async_write(
											*stream, *res, asio::redirect_error(asio::use_awaitable, ec));

										boost::system::error_code shutdownEc;
										stream->socket().shutdown(tcp::socket::shutdown_send, shutdownEc);
									},
									asio::detached);
							});
					}
				});
		}
		catch (const std::exception& e)
		{
			std::println(stderr, "Error handling request: {}", ec.message());
			std::fflush(stderr);
		}
	}

	asio::awaitable<void> accept(tcp::acceptor& acceptor)
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

			asio::co_spawn(
				acceptor.get_executor(),
				[socketPtr = std::make_shared<tcp::socket>(std::move(socket))]() -> asio::awaitable<void>
				{
					//
					co_await sessionHandler(std::move(*socketPtr));
				},
				asio::detached);
		}
	}

	asio::awaitable<void> runServer()
	{
		const auto [ip, port] = parseHostPort(Config::listenAddress, 8080);
		const auto endpoint = tcp::endpoint{boost::asio::ip::make_address(ip), port};

		tcp::acceptor acceptor(*ioc, endpoint);
		acceptor.set_option(asio::socket_base::reuse_address(true));
		acceptor.set_option(boost::asio::ip::tcp::no_delay(true));

		std::println("Server listening on {}", Config::listenAddress);
		std::fflush(stdout);

		co_await accept(acceptor);
	}

	void run()
	{
		ioc = std::make_unique<asio::io_context>(Config::ioWorkers);

		workerPool = std::make_unique<asio::thread_pool>(Config::handlerWorkers);

		asio::co_spawn(*ioc, runServer, asio::detached);

		std::vector<std::jthread> threads;
		threads.reserve(1 + Config::ioWorkers);

		if (Config::coordinator)
			threads.emplace_back(GatewayChooserService::start());

		threads.emplace_back(PaymentProcessor::start(*ioc, pendingPaymentsQueue, paymentService));

		getConnection();

		for (unsigned i = 1; i < Config::ioWorkers; ++i)
			threads.emplace_back([] { ioc->run(); });

		ioc->run();

		threads.clear();
		workerPool->stop();
		workerPool->join();

		std::println("Server stopped");
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
		std::println(stderr, "{}", e.what());
		std::fflush(stderr);
		return 1;
	}
}
