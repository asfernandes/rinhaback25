#pragma once

#include "Types.h"
#include <chrono>
#include <sstream>
#include <string>
#include <utility>
#include <cstdint>


namespace rinhaback
{
	inline const std::string HTTP_CONTENT_TYPE_JSON = "application/json";

	inline DateTimeMillis getCurrentDateTime()
	{
		return std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
	}

	inline DateTimeMillis parseDateTime(const std::string& str)
	{
		DateTimeMillis dateTime;

		std::istringstream iss{str};
		std::chrono::from_stream(iss, "%FT%T%Z", dateTime);

		if (iss.fail())
			throw std::invalid_argument("Invalid date time: " + str);

		return dateTime;
	}

	inline std::pair<std::string, uint16_t> parseHostPort(const std::string& hostPort, uint16_t defaultPort)
	{
		std::string host;
		uint16_t port = defaultPort;

		if (const auto pos = hostPort.find(':'); pos == std::string::npos)
			host = hostPort;
		else
		{
			host = hostPort.substr(0, pos);
			port = static_cast<unsigned short>(std::stoi(hostPort.substr(pos + 1)));
		}

		return {host, port};
	}
}  // namespace rinhaback
