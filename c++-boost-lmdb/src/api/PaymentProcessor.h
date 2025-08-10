#pragma once

#include "./PaymentService.h"
#include "./PendingPaymentsQueue.h"
#include <memory>
#include <thread>
#include "boost/asio.hpp"


namespace rinhaback::api
{
	class PaymentProcessor final
	{
	public:
		PaymentProcessor(boost::asio::io_context& ioc, std::shared_ptr<PendingPaymentsQueue> pendingPaymentsQueue,
			std::shared_ptr<PaymentService> paymentService)
			: ioc(ioc),
			  pendingPaymentsQueue(std::move(pendingPaymentsQueue)),
			  paymentService(std::move(paymentService))
		{
		}

		PaymentProcessor(const PaymentProcessor&) = delete;
		PaymentProcessor& operator=(const PaymentProcessor&) = delete;

	public:
		static std::jthread start(boost::asio::io_context& ioc,
			std::shared_ptr<PendingPaymentsQueue> pendingPaymentsQueue, std::shared_ptr<PaymentService> paymentService);

	private:
		boost::asio::awaitable<void> handler();
		boost::asio::awaitable<void> processPayment(const PendingPaymentsQueue::Payment& payment);

	private:
		boost::asio::io_context& ioc;
		std::shared_ptr<PendingPaymentsQueue> pendingPaymentsQueue;
		std::shared_ptr<PaymentService> paymentService;
	};
}  // namespace rinhaback::api
