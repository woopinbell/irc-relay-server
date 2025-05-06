#include "IrcApplication.hpp"

#include "Connection.hpp"
#include "Replies.hpp"

#include <sstream>
#include <utility>
#include <vector>

namespace {
    std::string joinWords(const std::vector<std::string>& values, const std::string& separator) {
        std::ostringstream out;
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i != 0) {
                out << separator;
            }
            out << values[i];
        }
        return out.str();
    }
}

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

bool IrcApplication::isChannelTarget(const std::string& target) {
    return !target.empty() && (target[0] == '#' || target[0] == '&');
}

Channel& IrcApplication::ensureChannel(const std::string& name) {
    std::map<std::string, Channel>::iterator it = _channels.find(name);
    if (it == _channels.end()) {
        it = _channels.insert(std::make_pair(name, Channel(name))).first;
    }
    return it->second;
}

Channel* IrcApplication::findChannelForCommand(int fd, const std::string& name, bool requireMembership) {
    std::map<std::string, Channel>::iterator it = _channels.find(name);
    if (it == _channels.end()) {
        sendNumeric(fd, 403, std::vector<std::string>(1, name), "No such channel");
        return NULL;
    }
    if (requireMembership && !it->second.hasMember(fd)) {
        sendNumeric(fd, 442, std::vector<std::string>(1, name), "You're not on that channel");
        return NULL;
    }
    return &it->second;
}

void IrcApplication::sendTopicReply(int fd, const Channel& channel) {
    if (channel.hasTopic()) {
        sendNumeric(fd, 332, std::vector<std::string>(1, channel.name()), channel.topic());
    } else {
        sendNumeric(fd, 331, std::vector<std::string>(1, channel.name()), "No topic is set");
    }
}

void IrcApplication::sendNames(int fd, const Channel& channel) {
    std::vector<std::string> names;
    const std::vector<int> members = channel.members();
    for (std::size_t i = 0; i < members.size(); ++i) {
        const ClientState* client = _clients.find(members[i]);
        if (client == NULL) {
            continue;
        }
        names.push_back((channel.isOperator(members[i]) ? "@" : "") + client->nick);
    }

    std::vector<std::string> nameParams;
    nameParams.push_back("=");
    nameParams.push_back(channel.name());
    sendNumeric(fd, 353, nameParams, joinWords(names, " "));
    sendNumeric(fd, 366, std::vector<std::string>(1, channel.name()), "End of /NAMES list");
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
