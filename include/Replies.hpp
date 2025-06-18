#ifndef REPLIES_HPP
#define REPLIES_HPP

#include <string>
#include <vector>

namespace Replies {
    std::string code(int numeric);
    std::string formatMessage(const std::string& prefix,
                              const std::string& command,
                              const std::vector<std::string>& params);
    std::string numeric(const std::string& serverName,
                        const std::string& target,
                        int numericCode,
                        const std::vector<std::string>& params,
                        const std::string& trailing);
    std::string error(const std::string& message);
    std::string hostmask(const std::string& nick, const std::string& user, const std::string& host);
}

#endif
