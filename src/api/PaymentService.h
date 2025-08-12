#pragma once

#include "./Database.h"
#include "./PaymentRepository.h"
#include "../common/Types.h"
#include "../common/Util.h"
#include <optional>
#include <utility>


namespace rinhaback::api
{
	class PaymentService final
	{
	public:
		PaymentService() = default;

		PaymentService(const PaymentService&) = delete;
		PaymentService& operator=(const PaymentService&) = delete;

	public:
		void postPayment(
			PaymentGateway gateway, double amount, const CorrelationId& correlationId, DateTimeMillis requestedAt);
		PaymentsSummaryResponse getPaymentsSummary(
			std::optional<DateTimeMillis> from, std::optional<DateTimeMillis> to);

		void purge();

	private:
		PaymentRepository repositories[std::to_underlying(PaymentGateway::SIZE)] = {
			{PaymentGateway::DEFAULT}, {PaymentGateway::FALLBACK}};
	};
}  // namespace rinhaback::api
