#pragma once

#include "./Database.h"
#include "../common/Types.h"
#include "../common/Util.h"
#include <optional>
#include <cstdint>


namespace rinhaback::api
{
	class PaymentRepository final
	{
	private:
		struct __attribute__((packed)) PaymentKey final
		{
			std::int64_t dateTime;
		};

		struct __attribute__((packed)) PaymentData final
		{
			double amount;
			CorrelationId correlationId;
		};

	public:
		PaymentRepository(PaymentGateway gateway)
			: gateway(gateway)
		{
		}

		PaymentRepository(const PaymentRepository&) = delete;
		PaymentRepository& operator=(const PaymentRepository&) = delete;

	public:
		void postPayment(double amount, const CorrelationId& correlationId, DateTimeMillis requestedAt);

		PaymentsGatewaySummaryResponse getPaymentsSummary(
			Transaction& transaction, std::optional<std::int64_t> from, std::optional<std::int64_t> to);

		void purge();

	private:
		PaymentGateway gateway;
	};
}  // namespace rinhaback::api
