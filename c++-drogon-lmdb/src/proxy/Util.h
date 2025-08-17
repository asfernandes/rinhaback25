#pragma once

#include <string>
#include <utility>
#include <cstdint>


namespace rinhaback::proxy
{
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
}  // namespace rinhaback::proxy
