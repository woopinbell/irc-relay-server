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

bool IrcMessage::parseLine(const std::string& line, IrcMessage& out, std::string* error) {
    const std::string trimmed = trimFrame(line);
    out = IrcMessage();
    out.raw = trimmed;

    if (trimmed.empty()) {
        if (error) {
            *error = "empty IRC frame";
        }
        return false;
    }
    if (trimmed.size() > 510) {
        if (error) {
            *error = "IRC frame exceeds 510 octets before CRLF";
        }
        return false;
    }

    std::size_t pos = 0;
    if (trimmed[pos] == ':') {
        const std::size_t end = trimmed.find(' ');
        if (end == std::string::npos || end == 1) {
            if (error) {
                *error = "message prefix is missing a command";
            }
            return false;
        }
        out.prefix = trimmed.substr(1, end - 1);
        pos = end + 1;
    }

    while (pos < trimmed.size() && trimmed[pos] == ' ') {
        ++pos;
    }

    const std::size_t commandStart = pos;
    while (pos < trimmed.size() && trimmed[pos] != ' ') {
        ++pos;
    }
    if (commandStart == pos) {
        if (error) {
            *error = "IRC command is missing";
        }
        return false;
    }
    out.command = upper(trimmed.substr(commandStart, pos - commandStart));

    while (pos < trimmed.size()) {
        while (pos < trimmed.size() && trimmed[pos] == ' ') {
            ++pos;
        }
        if (pos >= trimmed.size()) {
            break;
        }
        if (trimmed[pos] == ':') {
            out.params.push_back(trimmed.substr(pos + 1));
            break;
        }
        const std::size_t paramStart = pos;
        while (pos < trimmed.size() && trimmed[pos] != ' ') {
            ++pos;
        }
        out.params.push_back(trimmed.substr(paramStart, pos - paramStart));
    }

    return true;
}

std::string IrcMessage::upper(const std::string& value) {
    std::string copy = value;
    std::transform(copy.begin(), copy.end(), copy.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return copy;
}
