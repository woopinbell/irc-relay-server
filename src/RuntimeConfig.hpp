#ifndef IRC_RUNTIME_CONFIG_HPP
#define IRC_RUNTIME_CONFIG_HPP

#include "Server.hpp"

#include <cstddef>
#include <string>

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

private:
    static int parsePositiveInt(const std::string& value, const std::string& name);
    static bool startsWith(const std::string& value, const std::string& prefix);
};

#endif // IRC_RUNTIME_CONFIG_HPP
