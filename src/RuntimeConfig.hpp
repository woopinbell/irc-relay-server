#ifndef IRC_RUNTIME_CONFIG_HPP
#define IRC_RUNTIME_CONFIG_HPP

#include "Server.hpp"

#include <cstddef>

class RuntimeConfig {
public:
    std::size_t rateLimitCount;
    int rateLimitWindowSeconds;
    int idleTimeoutSeconds;
    int pingTimeoutSeconds;
    int registrationTimeoutSeconds;

    RuntimeConfig();

    static void printUsage(const char* programName);
    static int parsePort(const char* value);
    static RuntimeConfig parseOptions(int argc, char** argv, Server::Config& serverConfig);
};

#endif // IRC_RUNTIME_CONFIG_HPP
