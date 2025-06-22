#include "IrcApplication.hpp"

#include "IrcMessage.hpp"

#include <cctype>
#include <vector>

namespace {
    bool isValidNickname(const std::string& nickname) {
        if (nickname.empty() || nickname.size() > 30) {
            return false;
        }
        const unsigned char first = static_cast<unsigned char>(nickname[0]);
        if (std::isdigit(first) || nickname[0] == '#' || nickname[0] == '&' || nickname[0] == ':' || nickname[0] == '-') {
            return false;
        }
        for (std::size_t i = 0; i < nickname.size(); ++i) {
            const unsigned char ch = static_cast<unsigned char>(nickname[i]);
            if (std::isspace(ch) || ch == ',' || ch == '*' || ch == '?' || ch == '!' || ch == '@') {
                return false;
            }
        }
        return true;
    }
}

void IrcApplication::handlePass(int fd, const IrcMessage& message) {
    ClientState& client = _clients.state(fd);
    if (client.registered || client.passOk) {
        sendNumeric(fd, 462, std::vector<std::string>(), "You may not reregister");
        return;
    }
    if (message.params.empty()) {
        sendNumeric(fd, 461, std::vector<std::string>(1, "PASS"), "Not enough parameters");
        return;
    }
    if (!_password.empty() && message.params[0] != _password) {
        sendNumeric(fd, 464, std::vector<std::string>(), "Password incorrect");
        requestClose(fd, "Password incorrect");
        return;
    }
    client.passOk = true;
    maybeRegister(fd);
}

void IrcApplication::handleNick(int fd, const IrcMessage& message) {
    if (message.params.empty()) {
        sendNumeric(fd, 431, std::vector<std::string>(), "No nickname given");
        return;
    }

    const std::string nextNick = message.params[0];
    if (!isValidNickname(nextNick)) {
        sendNumeric(fd, 432, std::vector<std::string>(1, nextNick), "Erroneous nickname");
        return;
    }

    const int collision = _clients.findFdByNickname(nextNick);
    if (collision != -1 && collision != fd) {
        sendNumeric(fd, 433, std::vector<std::string>(1, nextNick), "Nickname is already in use");
        return;
    }

    _clients.setNickname(fd, nextNick);
    maybeRegister(fd);
}

void IrcApplication::handleUser(int fd, const IrcMessage& message) {
    ClientState& client = _clients.state(fd);
    if (client.registered) {
        sendNumeric(fd, 462, std::vector<std::string>(), "You may not reregister");
        return;
    }
    if (message.params.size() < 4) {
        sendNumeric(fd, 461, std::vector<std::string>(1, "USER"), "Not enough parameters");
        return;
    }
    client.user = message.params[0];
    client.realname = message.params[3];
    client.hasUser = true;
    maybeRegister(fd);
}

void IrcApplication::maybeRegister(int fd) {
    ClientState& client = _clients.state(fd);
    if (client.registered || !client.passOk || !client.hasNick || !client.hasUser) {
        return;
    }
    client.registered = true;
    sendNumeric(fd, 1, std::vector<std::string>(), "Welcome to the educational IRC reference, " + client.nick);
    sendNumeric(fd, 2, std::vector<std::string>(), "Your host is " + _serverName + ", running a C++17 reference server");
    sendNumeric(fd, 3, std::vector<std::string>(), "This server was created for IRC protocol learning");
}
