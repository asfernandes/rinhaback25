#include "mimalloc-new-delete.h"
#include "./PaymentProcessor.h"
#include "./Config.h"
#include "./GatewayChooserService.h"
#include "./PaymentService.h"
#include "./PendingPaymentsQueue.h"
#include "./SignalHandling.h"
#include "../common/Protocol.h"
#include "../common/Util.h"
#include <atomic>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <thread>
#include <vector>
#include "boost/asio.hpp"

namespace asio = boost::asio;
namespace ipc = boost::interprocess;


namespace
{
	using namespace rinhaback;
	using namespace rinhaback::api;

	std::unique_ptr<IpcConnection> ipcConnection;
	std::shared_ptr<PaymentService> paymentService{std::make_shared<PaymentService>()};
	std::shared_ptr<PendingPaymentsQueue> pendingPaymentsQueue{std::make_shared<PendingPaymentsQueue>()};

	void handler()
	{
		static std::atomic_uint handlerCounter;
		const auto handlerId = handlerCounter++;

		auto ipcMessage = &ipcConnection->header->items[handlerId];

		do
		{
			ipcMessage->requestReady.wait();

			switch (ipcMessage->messageType)
			{
				case IpcMessageType::REQUEST_POST_PAYMENT:
				{
					const PendingPaymentsQueue::Payment payment{.amount = ipcMessage->postPaymentRequest.amount,
						.correlationId = ipcMessage->postPaymentRequest.correlationId};

					pendingPaymentsQueue->enqueue(payment);
					break;
				}

				case IpcMessageType::REQUEST_PAYMENTS_SUMMARY:
					ipcMessage->paymentsSummaryResponse = paymentService->getPaymentsSummary(
						ipcMessage->paymentsSummaryRequest.from, ipcMessage->paymentsSummaryRequest.to);
					ipcMessage->responseReady.post();
					break;

				case IpcMessageType::REQUEST_PURGE_PAYMENTS:
					paymentService->purge();
					ipcMessage->responseReady.post();
					break;

				default:
					ipcMessage->responseReady.post();
					break;
			}
		} while (true);
	}

	void run()
	{
		asio::io_context ioc;
		std::vector<std::jthread> threads;

		if (Config::coordinator)
			threads.emplace_back(GatewayChooserService::start());

		threads.emplace_back(PaymentProcessor::start(ioc, pendingPaymentsQueue, paymentService));

		getConnection();

		ipcConnection =
			std::make_unique<IpcConnection>(Config::coordinator ? std::optional{Config::workers} : std::nullopt);

		for (unsigned i = 0; i < Config::workers; ++i)
			threads.emplace_back([] { handler(); });

		ioc.run();

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
}
