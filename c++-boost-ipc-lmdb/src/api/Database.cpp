#include "./Database.h"
#include "./Config.h"
#include "../common/Util.h"
#include <bit>
#include <filesystem>
#include <thread>
#include <cstring>
#include "boost/interprocess/sync/named_semaphore.hpp"

namespace stdfs = std::filesystem;
namespace boostipc = boost::interprocess;


namespace rinhaback::api
{
	static constexpr const char* SHARED_COORDINATOR_SEMAPHORE_NAME = "rinhaback25-boost-lmdb-Coordinator";
	static boostipc::named_semaphore ready{boostipc::open_or_create, SHARED_COORDINATOR_SEMAPHORE_NAME, 0};

	Connection::Connection()
	{
		if (Config::coordinator)
		{
			if (stdfs::exists(Config::database))
			{
				stdfs::remove(stdfs::path(Config::database).append("data.mdb"));
				stdfs::remove(stdfs::path(Config::database).append("lock.mdb"));
			}
			else
				stdfs::create_directories(Config::database);
		}
		else
		{
			ready.wait();

			std::println("Database initialized by other process.");
			std::fflush(stdout);
		}

		const int endiannessFlags = std::endian::native == std::endian::little ? (MDB_REVERSEKEY | MDB_REVERSEDUP) : 0;

		checkMdbError(mdb_env_create(&env));
		checkMdbError(mdb_env_set_mapsize(env, Config::databaseSize));
		checkMdbError(mdb_env_set_maxdbs(env, std::to_underlying(PaymentGateway::SIZE)));
		checkMdbError(mdb_env_open(env, Config::database.c_str(),
			MDB_WRITEMAP | MDB_NOMETASYNC | MDB_NOSYNC | MDB_NOTLS | MDB_NOMEMINIT |
				(Config::coordinator ? MDB_CREATE : 0),
			0664));

		Transaction transaction(*this, 0);

		checkMdbError(
			mdb_dbi_open(transaction.txn, "default", MDB_CREATE | MDB_DUPSORT | MDB_DUPFIXED | endiannessFlags,
				&dbis[std::to_underlying(PaymentGateway::DEFAULT)]));

		checkMdbError(
			mdb_dbi_open(transaction.txn, "fallback", MDB_CREATE | MDB_DUPSORT | MDB_DUPFIXED | endiannessFlags,
				&dbis[std::to_underlying(PaymentGateway::FALLBACK)]));

		if (Config::coordinator)
		{
			std::println("Database initialized.");
			std::fflush(stdout);

			ready.post();
		}
	}

	Connection::~Connection()
	{
		for (auto dbi : dbis)
		{
			if (dbi)
				mdb_dbi_close(env, dbi);
		}

		mdb_env_close(env);
	}
}  // namespace rinhaback::api
