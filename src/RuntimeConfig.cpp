#include "RuntimeConfig.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

template <typename Unsigned>
Unsigned parseUnsignedDecimal(const std::string& value,
                              Unsigned maximum,
                              const std::string& errorMessage) {
    if (value.empty()) {
        throw std::runtime_error(errorMessage);
    }

    Unsigned parsed = 0;
    for (std::string::size_type i = 0; i < value.size(); ++i) {
        if (value[i] < '0' || value[i] > '9') {
            throw std::runtime_error(errorMessage);
        }

        const Unsigned digit = static_cast<Unsigned>(value[i] - '0');
        if (parsed > maximum / 10
            || (parsed == maximum / 10 && digit > maximum % 10)) {
            throw std::runtime_error(errorMessage);
        }
        parsed = static_cast<Unsigned>(parsed * 10 + digit);
    }
    return parsed;
}

} // namespace

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
              << "[--rate-limit=COUNT:SECONDS] [--max-pending-bytes=N] [--max-connections=N]"
              << std::endl;
}

int RuntimeConfig::parsePort(const char* value) {
    const std::string text(value == NULL ? "" : value);
    const unsigned short port = parseUnsignedDecimal<unsigned short>(
        text,
        std::numeric_limits<unsigned short>::max(),
        "port must be an integer from 1 to 65535");
    if (port == 0) {
        throw std::runtime_error("port must be an integer from 1 to 65535");
    }
    return static_cast<int>(port);
}

RuntimeConfig RuntimeConfig::parseOptions(int argc, char** argv, Server::Config& serverConfig) {
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
        } else if (startsWith(arg, "--max-pending-bytes=")) {
            serverConfig.maxPendingBytes = parseSize(arg.substr(20), "max pending bytes");
        } else if (startsWith(arg, "--max-connections=")) {
            serverConfig.maxConnections = parseSize(arg.substr(18), "max connections");
        } else {
            throw std::runtime_error("unknown option: " + arg);
        }
    }
    return runtime;
}

std::size_t RuntimeConfig::parseSize(const std::string& value, const std::string& name) {
    return parseUnsignedDecimal<std::size_t>(
        value,
        std::numeric_limits<std::size_t>::max(),
        name + " must be an unsigned integer");
}

int RuntimeConfig::parsePositiveInt(const std::string& value, const std::string& name) {
    const unsigned int parsed = parseUnsignedDecimal<unsigned int>(
        value,
        86400U,
        name + " must be a positive integer");
    if (parsed == 0) {
        throw std::runtime_error(name + " must be a positive integer");
    }
    return static_cast<int>(parsed);
}

bool RuntimeConfig::startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}
