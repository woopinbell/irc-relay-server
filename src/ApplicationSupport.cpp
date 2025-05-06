#include "IrcApplication.hpp"

#include "Connection.hpp"
#include "Replies.hpp"

#include <vector>

std::vector<std::string> IrcApplication::splitComma(const std::string& value) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t comma = value.find(',', start);
        const std::size_t end = comma == std::string::npos ? value.size() : comma;
        if (end > start) {
            parts.push_back(value.substr(start, end - start));
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return parts;
}

int IrcApplication::findNick(const std::string& nickname) const {
    return _clients.findFdByNickname(nickname);
}

std::string IrcApplication::replyTarget(int fd) const {
    const ClientState* client = _clients.find(fd);
    if (client == NULL || client->nick.empty()) {
        return "*";
    }
    return client->nick;
}

std::string IrcApplication::prefixFor(int fd) const {
    const ClientState* client = _clients.find(fd);
    if (client == NULL) {
        return _serverName;
    }
    return prefixFor(*client);
}

std::string IrcApplication::prefixFor(const ClientState& client) const {
    if (client.nick.empty()) {
        return _serverName;
    }
    return Replies::hostmask(client.nick, client.user, client.host);
}

void IrcApplication::sendNumeric(int fd, int numericCode, const std::vector<std::string>& params, const std::string& trailing) {
    sendRaw(fd, Replies::numeric(_serverName, replyTarget(fd), numericCode, params, trailing));
}

void IrcApplication::sendNumericRaw(int fd, int numericCode, const std::vector<std::string>& params) {
    std::vector<std::string> allParams;
    allParams.push_back(replyTarget(fd));
    allParams.insert(allParams.end(), params.begin(), params.end());
    sendRaw(fd, Replies::formatMessage(_serverName, Replies::code(numericCode), allParams));
}

void IrcApplication::sendRaw(int fd, const std::string& line) {
    _server.sendTo(fd, line);
}

void IrcApplication::requestClose(int fd, const std::string& reason) {
    Connection* connection = _server.findConnection(fd);
    if (connection != NULL) {
        connection->requestClose(reason);
    }
}

void IrcApplication::removeClientState(int fd, const std::string&, bool) {
    _clients.erase(fd);
}
