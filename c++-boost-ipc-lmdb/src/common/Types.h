#pragma once

#include <array>
#include <chrono>


namespace rinhaback
{
	using CorrelationId = std::array<char, 36>;
	using DateTimeMillis = std::chrono::sys_time<std::chrono::milliseconds>;

	struct PaymentsGatewaySummaryResponse final
	{
		unsigned totalRequests;
		double totalAmount;
	};

	struct PaymentsSummaryResponse final
	{
		PaymentsGatewaySummaryResponse defaultGateway;
		PaymentsGatewaySummaryResponse fallbackGateway;
	};
}  // namespace rinhaback
