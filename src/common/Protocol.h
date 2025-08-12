#pragma once

#include "./Types.h"
#include <mutex>
#include <optional>
#include <print>
#include <cstdint>
#include "boost/interprocess/mapped_region.hpp"
#include "boost/interprocess/shared_memory_object.hpp"
#include "boost/interprocess/sync/interprocess_condition.hpp"
#include "boost/interprocess/sync/interprocess_mutex.hpp"
#include "boost/interprocess/sync/interprocess_semaphore.hpp"


namespace rinhaback
{
	enum class IpcMessageType : uint8_t
	{
		REQUEST_POST_PAYMENT,
		REQUEST_PAYMENTS_SUMMARY,
		REQUEST_PURGE_PAYMENTS
	};

	struct IpcMessage
	{
		IpcMessage() { }

		boost::interprocess::interprocess_semaphore requestReady{0};
		boost::interprocess::interprocess_semaphore responseReady{0};

		IpcMessageType messageType;

		union
		{
			struct
			{
				CorrelationId correlationId;
				double amount;
			} postPaymentRequest;

			struct
			{
				std::optional<DateTimeMillis> from;
				std::optional<DateTimeMillis> to;
			} paymentsSummaryRequest;

			PaymentsSummaryResponse paymentsSummaryResponse;
		};
	};

	struct IpcHeader
	{
		bool ready = false;
		boost::interprocess::interprocess_mutex readyMutex;
		boost::interprocess::interprocess_condition readyCondition;
		IpcMessage items[];
	};

	class IpcConnection
	{
	private:
		static constexpr const char* SHARED_MEMORY_NAME = "rinhaback25-boost-lmdb-ipc-connection";

	public:
		explicit IpcConnection(std::optional<unsigned> workersCount)
		{
			if (workersCount.has_value())
			{
				boost::interprocess::shared_memory_object::remove(SHARED_MEMORY_NAME);
				shm = boost::interprocess::shared_memory_object(
					boost::interprocess::create_only, SHARED_MEMORY_NAME, boost::interprocess::read_write);

				shm.truncate(sizeof(IpcHeader) + sizeof(IpcMessage) * workersCount.value());

				region = boost::interprocess::mapped_region(shm, boost::interprocess::read_write);
				header = new (region.get_address()) IpcHeader();

				for (unsigned i = 0; i < workersCount.value(); ++i)
					new (&header->items[i]) IpcMessage();

				std::println("IPC connection initialized.");
				std::fflush(stdout);

				std::unique_lock lock(header->readyMutex);
				header->ready = true;
				header->readyCondition.notify_all();
			}
			else
			{
				shm = boost::interprocess::shared_memory_object(
					boost::interprocess::open_only, SHARED_MEMORY_NAME, boost::interprocess::read_write);
				region = boost::interprocess::mapped_region(shm, boost::interprocess::read_write);
				header = static_cast<IpcHeader*>(region.get_address());

				std::unique_lock lock(header->readyMutex);
				header->readyCondition.wait(lock, [&] { return header->ready; });

				std::println("IPC connection initialized by other process.");
				std::fflush(stdout);
			}
		}

	public:
		IpcHeader* header;

	private:
		boost::interprocess::shared_memory_object shm;
		boost::interprocess::mapped_region region;
	};
}  // namespace rinhaback
