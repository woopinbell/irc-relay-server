#include "Replies.hpp"

#include <iomanip>
#include <sstream>

namespace {
    bool needsTrailingMarker(const std::string& value) {
        return value.empty() || value.find(' ') != std::string::npos || value[0] == ':';
    }
}

std::string Replies::code(int numeric) {
    std::ostringstream out;
    out << std::setfill('0') << std::setw(3) << numeric;
    return out.str();
}

std::string Replies::formatMessage(const std::string& prefix,
                                   const std::string& command,
                                   const std::vector<std::string>& params) {
    std::ostringstream out;
    if (!prefix.empty()) {
        out << ':' << prefix << ' ';
    }
    out << command;
    for (std::size_t i = 0; i < params.size(); ++i) {
        out << ' ';
        if (i + 1 == params.size() && needsTrailingMarker(params[i])) {
            out << ':' << params[i];
        } else {
            out << params[i];
        }
    }
    out << "\r\n";
    return out.str();
}

std::string Replies::numeric(const std::string& serverName,
                             const std::string& target,
                             int numericCode,
                             const std::vector<std::string>& params,
                             const std::string& trailing) {
    std::vector<std::string> allParams;
    allParams.push_back(target.empty() ? "*" : target);
    allParams.insert(allParams.end(), params.begin(), params.end());
    allParams.push_back(trailing);
    return formatMessage(serverName, code(numericCode), allParams);
}

std::string Replies::error(const std::string& message) {
    return formatMessage("", "ERROR", std::vector<std::string>(1, message));
}

std::string Replies::hostmask(const std::string& nick, const std::string& user, const std::string& host) {
    const std::string safeUser = user.empty() ? "unknown" : user;
    const std::string safeHost = host.empty() ? "localhost" : host;
    return nick + "!" + safeUser + "@" + safeHost;
}
