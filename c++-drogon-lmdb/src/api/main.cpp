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
#include "boost/json.hpp"
#include "drogon/drogon.h"


namespace asio = boost::asio;
using tcp = asio::ip::tcp;


namespace
{
	using namespace rinhaback::api;

	std::shared_ptr<PaymentService> paymentService{std::make_shared<PaymentService>()};
	std::shared_ptr<PendingPaymentsQueue> pendingPaymentsQueue{std::make_shared<PendingPaymentsQueue>()};

	void paymentsSummaryHandler(
		const drogon::HttpRequestPtr& request, std::function<void(const drogon::HttpResponsePtr&)>&& callback)
	{
		std::optional<DateTimeMillis> from, to;

		if (const auto fromParam = request->getOptionalParameter<std::string>("from"))
			from = parseDateTime(fromParam.value());

		if (const auto toParam = request->getOptionalParameter<std::string>("to"))
			to = parseDateTime(toParam.value());

		const auto summary = paymentService->getPaymentsSummary(from, to);

		const auto& defaultGateway = summary.defaultGateway;
		const auto& fallbackGateway = summary.fallbackGateway;

		auto body = std::format(R"({{"default":{{"totalRequests":{},"totalAmount":{:.2f}}},)"
								R"("fallback":{{"totalRequests":{},"totalAmount":{:.2f}}}}})",
			defaultGateway.totalRequests, defaultGateway.totalAmount, fallbackGateway.totalRequests,
			fallbackGateway.totalAmount);

		auto response = drogon::HttpResponse::newHttpResponse();
		response->setBody(std::move(body));
		callback(response);
	}

	void postPaymentHandler(
		const drogon::HttpRequestPtr& request, std::function<void(const drogon::HttpResponsePtr&)>&& callback)
	{
		auto inJsonObj = boost::json::parse(request->body()).as_object();
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

				callback(drogon::HttpResponse::newHttpResponse());
				return;
			}
		}

		auto response = drogon::HttpResponse::newHttpResponse();
		response->setStatusCode(drogon::HttpStatusCode::k400BadRequest);
		callback(response);
	}

	void purgePaymentsHandler(
		const drogon::HttpRequestPtr& request, std::function<void(const drogon::HttpResponsePtr&)>&& callback)
	{
		paymentService->purge();
		pendingPaymentsQueue->purge();

		callback(drogon::HttpResponse::newHttpResponse());
	}

	void run()
	{
		std::vector<std::jthread> threads;

		if (Config::coordinator)
			threads.emplace_back(GatewayChooserService::start());

		threads.emplace_back(
			[]
			{
				asio::io_context processorIoc;
				PaymentProcessor::start(processorIoc, pendingPaymentsQueue, paymentService);
				processorIoc.run();
			});

		getConnection();

		const auto [ip, port] = parseHostPort(Config::listenAddress, 8080);

		drogon::app().registerHandler("/payments-summary",
			[](const drogon::HttpRequestPtr& request, std::function<void(const drogon::HttpResponsePtr&)>&& callback)
			{ paymentsSummaryHandler(request, std::move(callback)); }, {drogon::Get});

		drogon::app().registerHandler("/payments",
			[](const drogon::HttpRequestPtr& request, std::function<void(const drogon::HttpResponsePtr&)>&& callback)
			{ postPaymentHandler(request, std::move(callback)); }, {drogon::Post});

		drogon::app().registerHandler("/purge-payments",
			[](const drogon::HttpRequestPtr& request, std::function<void(const drogon::HttpResponsePtr&)>&& callback)
			{ purgePaymentsHandler(request, std::move(callback)); }, {drogon::Post});

		drogon::app().disableSession().addListener(ip, port).setThreadNum(Config::ioWorkers).run();

		threads.clear();

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

	return 0;
}
