#include "RuntimeConfig.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

RuntimeConfig::RuntimeConfig()
    : rateLimitCount(24),
      rateLimitWindowSeconds(3),
      idleTimeoutSeconds(120),
      pingTimeoutSeconds(30),
      registrationTimeoutSeconds(30) {
}

void RuntimeConfig::printUsage(const char* programName) {
    std::cerr << "Usage: " << programName << " <port> <password>" << std::endl;
}

int RuntimeConfig::parsePort(const char* value) {
    char* end = NULL;
    const long port = std::strtol(value, &end, 10);
    if (!value[0] || *end != '\0' || port <= 0 || port > 65535) {
        throw std::runtime_error("port must be an integer from 1 to 65535");
    }
    return static_cast<int>(port);
}

RuntimeConfig RuntimeConfig::parseOptions(int argc, char** argv, Server::Config& serverConfig) {
    (void)serverConfig;
    if (argc > 3) {
        throw std::runtime_error(std::string("unknown option: ") + argv[3]);
    }
    return RuntimeConfig();
}
