#include "./PaymentProcessor.h"
#include "./Config.h"
#include "./GatewayChooserService.h"
#include "./SignalHandling.h"
#include "./Util.h"
#include <format>
#include <mutex>
#include <print>
#include <string>
#include <string_view>
#include <cassert>
#include "boost/asio.hpp"
#include "boost/beast.hpp"
#include "boost/url.hpp"
#include "boost/url/parse.hpp"


namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
using tcp = asio::ip::tcp;


namespace rinhaback::api
{
	static std::once_flag resolverOnce;
	static tcp::endpoint defaultEndpoint, fallbackEndpoint;

	std::jthread PaymentProcessor::start(boost::asio::io_context& ioc,
		std::shared_ptr<PendingPaymentsQueue> pendingPaymentsQueue, std::shared_ptr<PaymentService> paymentService)
	{
		const auto processor =
			std::make_shared<PaymentProcessor>(ioc, std::move(pendingPaymentsQueue), std::move(paymentService));

		return std::jthread(
			[processor]()
			{
				asio::co_spawn(processor->ioc, processor->handler(), asio::detached);
				processor->ioc.run();
			});
	}

	boost::asio::awaitable<void> PaymentProcessor::handler()
	{
		std::println("PaymentProcessor started.");
		std::fflush(stdout);

		std::call_once(resolverOnce,
			[&]
			{
				tcp::resolver resolver{ioc};

				{  // scope
					const auto [host, port] = parseHostPort(Config::processorDefaultAddress, 8080);
					defaultEndpoint = resolver.resolve(host, std::to_string(port)).begin()->endpoint();
				}

				{  // scope
					const auto [host, port] = parseHostPort(Config::processorFallbackAddress, 8080);
					fallbackEndpoint = resolver.resolve(host, std::to_string(port)).begin()->endpoint();
				}
			});

		while (!SignalHandling::shouldFinish())
		{
			const auto optionalPayment = pendingPaymentsQueue->dequeue();

			if (optionalPayment.has_value())
				co_await processPayment(optionalPayment.value());
		}

		std::println("PaymentProcessor stopped.");
		std::fflush(stdout);
	}

	boost::asio::awaitable<void> PaymentProcessor::processPayment(const PendingPaymentsQueue::Payment& payment)
	{
		const auto gateway = GatewayChooserService::getGateway();
		const std::string* host = nullptr;
		const tcp::endpoint* endpoint = nullptr;

		switch (gateway)
		{
			case PaymentGateway::DEFAULT:
				host = &Config::processorDefaultAddress;
				endpoint = &defaultEndpoint;
				break;

			case PaymentGateway::FALLBACK:
				host = &Config::processorFallbackAddress;
				endpoint = &fallbackEndpoint;
				break;

			default:
				assert(false);
				co_return;
		}

		auto stream = std::make_shared<beast::tcp_stream>(ioc);

		try
		{
			co_await stream->async_connect(*endpoint, asio::use_awaitable);

			const auto requestedAt = getCurrentDateTime();
			const auto jsonBody = std::format(R"({{"correlationId":"{}","amount":{:.2f},"requestedAt":"{:%FT%T}Z"}})",
				std::string_view(payment.correlationId.data(), payment.correlationId.size()), payment.amount,
				requestedAt);

			auto req = std::make_shared<http::request<http::string_body>>();
			req->method(http::verb::post);
			req->target("/payments");
			req->set(http::field::host, *host);
			req->set(http::field::content_type, HTTP_CONTENT_TYPE_JSON);
			req->body() = jsonBody;
			req->prepare_payload();

			co_await http::async_write(*stream, *req, asio::use_awaitable);

			auto buffer = std::make_shared<beast::flat_buffer>();
			auto res = std::make_shared<http::response<http::string_body>>();

			co_await http::async_read(*stream, *buffer, *res, asio::use_awaitable);

			boost::system::error_code shutdownEc;
			stream->socket().shutdown(tcp::socket::shutdown_both, shutdownEc);

			if (res->result() == http::status::ok)
			{
				if constexpr (false)
				{
					std::println("Payment processed successfully: correlationId: {}, amount: {}",
						std::string_view(payment.correlationId.data(), payment.correlationId.size()), payment.amount);
					std::fflush(stdout);
				}

				paymentService->postPayment(gateway, payment.amount, payment.correlationId, requestedAt);
			}
			else
			{
				GatewayChooserService::switchGatewayTo(
					gateway == PaymentGateway::DEFAULT ? PaymentGateway::FALLBACK : PaymentGateway::DEFAULT);

				if constexpr (false)
				{
					std::println("Payment processing failed: gateway: {}, correlationId: {}, amount: {}, "
								 "httpStatus: {}",
						gateway, std::string_view(payment.correlationId.data(), payment.correlationId.size()),
						payment.amount, (int) res->result());
					std::fflush(stdout);
				}

				// Try again with the other gateway
				co_await processPayment(payment);
			}
		}
		catch (const boost::system::system_error& e)
		{
			std::println(stderr, "Payment processing error: {}", e.what());
			std::fflush(stderr);
		}
	}
}  // namespace rinhaback::api
