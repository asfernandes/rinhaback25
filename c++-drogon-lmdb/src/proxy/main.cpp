#include "mimalloc-new-delete.h"
#include "./Config.h"
#include "./Util.h"
#include <atomic>
#include <functional>
#include <print>
#include <stdexcept>
#include <string>
#include "boost/asio.hpp"
#include "drogon/drogon.h"
#include "drogon/HttpClient.h"

namespace asio = boost::asio;
using tcp = asio::ip::tcp;


namespace
{
	using namespace rinhaback::proxy;

	struct Backend final
	{
		std::string address;
		std::string host;
		tcp::endpoint endpoint;
	};

	std::array<Backend, 2> backends;
	std::atomic<size_t> nextBackend{0};

	void resolveBackends()
	{
		asio::io_context ioc;
		tcp::resolver resolver{ioc};

		backends[0].address = Config::backend0Address;
		backends[1].address = Config::backend1Address;

		try
		{
			for (auto& backend : backends)
			{
				const auto [host, port] = parseHostPort(backend.address, 8080);
				auto results = resolver.resolve(host, std::to_string(port));
				backend.host = "http://" + backend.address;
				backend.endpoint = results.begin()->endpoint();
			}
		}
		catch (const std::exception& e)
		{
			std::println(stderr, "Failed to resolve backends: {}", e.what());
			std::fflush(stderr);
			throw;
		}
	}

	const Backend& getBackend()
	{
		size_t backendIndex = nextBackend.fetch_add(1) % backends.size();
		return backends[backendIndex];
	}

	void proxyRequest(
		const drogon::HttpRequestPtr& request, std::function<void(const drogon::HttpResponsePtr&)>&& callback)
	{
		const auto& backend = getBackend();

		// Create HTTP client for the target server
		auto client =
			drogon::HttpClient::newHttpClient(backend.endpoint.address().to_string(), backend.endpoint.port());

		// Create a new request to forward
		auto forwardReq = drogon::HttpRequest::newHttpRequest();
		forwardReq->setMethod(request->getMethod());
		forwardReq->setPath(request->getPath());

		// Copy query parameters
		auto& params = request->getParameters();
		for (const auto& param : params)
			forwardReq->setParameter(param.first, param.second);

		// Copy headers (excluding Host header which will be set by the client)
		auto& headers = request->getHeaders();
		for (const auto& header : headers)
		{
			if (header.first != "host" && header.first != "Host")
				forwardReq->addHeader(header.first, header.second);
		}

		// Copy request body if present
		if (request->getBody().length() > 0)
			forwardReq->setBody(std::string(request->getBody()));

		// Forward the request
		client->sendRequest(forwardReq,
			[callback = std::move(callback)](drogon::ReqResult result, const drogon::HttpResponsePtr& response)
			{
				if (result == drogon::ReqResult::Ok && response)
				{
					// Forward the response back to the client
					auto resp = drogon::HttpResponse::newHttpResponse();
					resp->setStatusCode(response->getStatusCode());
					resp->setBody(std::string(response->getBody()));

					// Copy response headers
					auto& responseHeaders = response->getHeaders();
					for (const auto& header : responseHeaders)
					{
						// Skip headers that might cause issues
						if (header.first != "transfer-encoding" && header.first != "Transfer-Encoding" &&
							header.first != "connection" && header.first != "Connection")
						{
							resp->addHeader(header.first, header.second);
						}
					}

					callback(resp);
				}
				else
				{
					// Handle error case
					auto errorResp = drogon::HttpResponse::newHttpResponse();
					errorResp->setStatusCode(drogon::HttpStatusCode::k502BadGateway);
					callback(errorResp);
				}
			});
	}

	void replyAndForwardRequest(
		const drogon::HttpRequestPtr& request, std::function<void(const drogon::HttpResponsePtr&)>&& callback)
	{
		callback(drogon::HttpResponse::newHttpResponse());

		const auto& backend = getBackend();

		// Create HTTP client for the target server
		auto client =
			drogon::HttpClient::newHttpClient(backend.endpoint.address().to_string(), backend.endpoint.port());

		// Create a new request to forward
		auto forwardReq = drogon::HttpRequest::newHttpRequest();
		forwardReq->setMethod(request->getMethod());
		forwardReq->setPath(request->getPath());

		// Copy query parameters
		auto& params = request->getParameters();
		for (const auto& param : params)
			forwardReq->setParameter(param.first, param.second);

		// Copy headers (excluding Host header which will be set by the client)
		auto& headers = request->getHeaders();
		for (const auto& header : headers)
		{
			if (header.first != "host" && header.first != "Host")
				forwardReq->addHeader(header.first, header.second);
		}

		// Copy request body if present
		if (request->getBody().length() > 0)
			forwardReq->setBody(std::string(request->getBody()));

		// Forward the request
		client->sendRequest(forwardReq, [](drogon::ReqResult result, const drogon::HttpResponsePtr& response) { });
	}

	void run()
	{
		resolveBackends();

		const auto [ip, port] = parseHostPort(Config::listenAddress, 8080);

		drogon::app().setDefaultHandler(
			[](const drogon::HttpRequestPtr& request, std::function<void(const drogon::HttpResponsePtr&)>&& callback)
			{ proxyRequest(request, std::move(callback)); });

		drogon::app().registerHandler("/payments",
			[](const drogon::HttpRequestPtr& request, std::function<void(const drogon::HttpResponsePtr&)>&& callback)
			{ replyAndForwardRequest(request, std::move(callback)); }, {drogon::Post});

		drogon::app().disableSession().addListener(ip, port).setThreadNum(Config::ioWorkers).run();
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
		std::println(stderr, "Fatal error: {}", e.what());
		std::fflush(stderr);
		return 1;
	}
}
