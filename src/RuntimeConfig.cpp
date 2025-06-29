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
    std::cerr << "Usage: " << programName << " <port> <password> "
              << "[--idle-timeout=N] [--ping-timeout=N] [--registration-timeout=N] "
              << "[--rate-limit=COUNT:SECONDS]" << std::endl;
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
    RuntimeConfig runtime;
    for (int i = 3; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (startsWith(arg, "--idle-timeout=")) {
            runtime.idleTimeoutSeconds = parsePositiveInt(arg.substr(15), "idle timeout");
        } else if (startsWith(arg, "--ping-timeout=")) {
            runtime.pingTimeoutSeconds = parsePositiveInt(arg.substr(15), "ping timeout");
        } else if (startsWith(arg, "--registration-timeout=")) {
            runtime.registrationTimeoutSeconds = parsePositiveInt(arg.substr(23), "registration timeout");
        } else if (startsWith(arg, "--rate-limit=")) {
            const std::string value = arg.substr(13);
            const std::string::size_type colon = value.find(':');
            if (colon == std::string::npos) {
                throw std::runtime_error("rate limit must use COUNT:SECONDS");
            }
            runtime.rateLimitCount = parseSize(value.substr(0, colon), "rate limit count");
            runtime.rateLimitWindowSeconds =
                parsePositiveInt(value.substr(colon + 1), "rate limit window");
        } else {
            throw std::runtime_error("unknown option: " + arg);
        }
    }
    return runtime;
}

std::size_t RuntimeConfig::parseSize(const std::string& value, const std::string& name) {
    char* end = NULL;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    if (value.empty() || *end != '\0') {
        throw std::runtime_error(name + " must be an unsigned integer");
    }
    return static_cast<std::size_t>(parsed);
}

int RuntimeConfig::parsePositiveInt(const std::string& value, const std::string& name) {
    char* end = NULL;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (value.empty() || *end != '\0' || parsed <= 0 || parsed > 86400) {
        throw std::runtime_error(name + " must be a positive integer");
    }
    return static_cast<int>(parsed);
}

bool RuntimeConfig::startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}
