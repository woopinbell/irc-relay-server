#include "IrcMessage.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

IrcMessage::IrcMessage() {
}

IrcMessage::IrcMessage(const std::string& messagePrefix,
                       const std::string& messageCommand,
                       const std::vector<std::string>& messageParams)
    : prefix(messagePrefix),
      command(upper(messageCommand)),
      params(messageParams) {
}

bool IrcMessage::isCommand(const std::string& name) const {
    return command == upper(name);
}

std::string IrcMessage::param(std::size_t index, const std::string& fallback) const {
    if (index >= params.size()) {
        return fallback;
    }
    return params[index];
}

std::string IrcMessage::toLine() const {
    std::ostringstream out;
    if (!prefix.empty()) {
        out << ':' << prefix << ' ';
    }
    out << command;
    for (std::size_t i = 0; i < params.size(); ++i) {
        out << ' ';
        const std::string& value = params[i];
        if (value.empty() || value.find(' ') != std::string::npos || value[0] == ':') {
            out << ':' << value;
        } else {
            out << value;
        }
    }
    out << "\r\n";
    return out.str();
}

std::string IrcMessage::upper(const std::string& value) {
    std::string copy = value;
    std::transform(copy.begin(), copy.end(), copy.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return copy;
}
