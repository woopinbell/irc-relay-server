#include "IrcApplication.hpp"

#include "Connection.hpp"
#include "Replies.hpp"

#include <cctype>
#include <iostream>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

AppMetrics::AppMetrics()
    : commandsHandled(0),
      messagesRelayed(0),
      roomsCreated(0),
      rateLimitedClients(0),
      idleTimeouts(0),
      heartbeatPings(0) {
}

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

    std::string logSafe(const std::string& value) {
        std::string copy = value;
        for (std::size_t i = 0; i < copy.size(); ++i) {
            const unsigned char ch = static_cast<unsigned char>(copy[i]);
            if (std::isspace(ch)) {
                copy[i] = '_';
            }
        }
        return copy;
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

void logEvent(const std::string& eventName, const std::vector<std::pair<std::string, std::string> >& fields) {
    std::cerr << "event=" << eventName;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        std::cerr << ' ' << fields[i].first << '=' << logSafe(fields[i].second);
    }
    std::cerr << std::endl;
}

void IrcApplication::handleMetrics(int fd) {
    const Server::Metrics& serverMetrics = _server.metrics();
    std::ostringstream out;
    out << "connections=" << _server.connectionCount()
        << " accepted=" << serverMetrics.acceptedConnections
        << " closed=" << serverMetrics.closedConnections
        << " rooms=" << _channels.size()
        << " commands=" << _metrics.commandsHandled
        << " messages=" << _metrics.messagesRelayed
        << " queue_drops=" << serverMetrics.outboundQueueDrops
        << " rate_limited=" << _metrics.rateLimitedClients;
    std::vector<std::string> params;
    params.push_back(replyTarget(fd));
    params.push_back(out.str());
    sendRaw(fd, Replies::formatMessage(_serverName, "NOTICE", params));
}

Channel& IrcApplication::ensureChannel(const std::string& name) {
    std::map<std::string, Channel>::iterator it = _channels.find(name);
    if (it == _channels.end()) {
        it = _channels.insert(std::make_pair(name, Channel(name))).first;
        ++_metrics.roomsCreated;
        logEvent("room_created", std::vector<std::pair<std::string, std::string> >{
            std::make_pair("name", name)
        });
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

bool IrcApplication::sendTopicReply(int fd, const Channel& channel) {
    const std::string channelName = channel.name();
    if (channel.hasTopic()) {
        return sendNumeric(fd, 332, std::vector<std::string>(1, channelName), channel.topic());
    }
    return sendNumeric(fd, 331, std::vector<std::string>(1, channelName), "No topic is set");
}

bool IrcApplication::sendNames(int fd, const Channel& channel) {
    const std::string channelName = channel.name();
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
    nameParams.push_back(channelName);
    if (!sendNumeric(fd, 353, nameParams, joinWords(names, " "))) {
        return false;
    }
    return sendNumeric(fd, 366, std::vector<std::string>(1, channelName), "End of /NAMES list");
}

void IrcApplication::partAllChannels(int fd, const std::string& reason) {
    std::vector<std::string> channelNames;
    for (std::map<std::string, Channel>::const_iterator it = _channels.begin(); it != _channels.end(); ++it) {
        if (it->second.hasMember(fd)) {
            channelNames.push_back(it->first);
        }
    }
    for (std::size_t i = 0; i < channelNames.size(); ++i) {
        partChannel(fd, channelNames[i], reason);
    }
}

void IrcApplication::partChannel(int fd, const std::string& channelName, const std::string& reason) {
    std::map<std::string, Channel>::iterator it = _channels.find(channelName);
    if (it == _channels.end()) {
        sendNumeric(fd, 403, std::vector<std::string>(1, channelName), "No such channel");
        return;
    }
    if (!it->second.hasMember(fd)) {
        sendNumeric(fd, 442, std::vector<std::string>(1, channelName), "You're not on that channel");
        return;
    }

    std::vector<std::string> params;
    params.push_back(channelName);
    if (!reason.empty()) {
        params.push_back(reason);
    }
    broadcastToChannel(channelName, Replies::formatMessage(prefixFor(fd), "PART", params), -1);
    it = _channels.find(channelName);
    if (it == _channels.end()) {
        return;
    }
    it->second.removeMember(fd);
    eraseChannelIfEmpty(channelName);
}

bool IrcApplication::broadcastMode(int fd, const Channel& channel, const std::string& mode, const std::string& arg) {
    const std::string channelName = channel.name();
    std::vector<std::string> params;
    params.push_back(channelName);
    params.push_back(mode);
    if (!arg.empty()) {
        params.push_back(arg);
    }
    broadcastToChannel(channelName, Replies::formatMessage(prefixFor(fd), "MODE", params), -1);
    return _clients.contains(fd) && _channels.find(channelName) != _channels.end();
}

void IrcApplication::broadcastToChannel(const std::string& channelName, const std::string& line, int exceptFd) {
    std::map<std::string, Channel>::const_iterator channelIt = _channels.find(channelName);
    if (channelIt == _channels.end()) {
        return;
    }

    const std::vector<int> members = channelIt->second.members();
    for (std::size_t i = 0; i < members.size(); ++i) {
        if (members[i] != exceptFd) {
            sendRaw(members[i], line);
        }
    }
}

void IrcApplication::broadcastToCommon(int fd, const std::string& line, bool includeSelf) {
    std::set<int> targets;
    for (std::map<std::string, Channel>::const_iterator it = _channels.begin(); it != _channels.end(); ++it) {
        if (!it->second.hasMember(fd)) {
            continue;
        }
        const std::vector<int> members = it->second.members();
        targets.insert(members.begin(), members.end());
    }
    if (!includeSelf) {
        targets.erase(fd);
    }
    for (std::set<int>::const_iterator it = targets.begin(); it != targets.end(); ++it) {
        sendRaw(*it, line);
    }
}

void IrcApplication::eraseChannelIfEmpty(const std::string& channelName) {
    std::map<std::string, Channel>::iterator it = _channels.find(channelName);
    if (it != _channels.end() && it->second.empty()) {
        _channels.erase(it);
    }
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

bool IrcApplication::sendNumeric(int fd, int numericCode, const std::vector<std::string>& params, const std::string& trailing) {
    return sendRaw(fd, Replies::numeric(_serverName, replyTarget(fd), numericCode, params, trailing));
}

bool IrcApplication::sendNumericRaw(int fd, int numericCode, const std::vector<std::string>& params) {
    std::vector<std::string> allParams;
    allParams.push_back(replyTarget(fd));
    allParams.insert(allParams.end(), params.begin(), params.end());
    return sendRaw(fd, Replies::formatMessage(_serverName, Replies::code(numericCode), allParams));
}

bool IrcApplication::sendRaw(int fd, const std::string& line) {
    return _server.sendTo(fd, line);
}

void IrcApplication::requestClose(int fd, const std::string& reason) {
    Connection* connection = _server.findConnection(fd);
    if (connection != NULL) {
        connection->requestClose(reason);
    }
}

void IrcApplication::removeClientState(int fd, const std::string& reason, bool notifyPeers) {
    const ClientState* found = _clients.find(fd);
    if (found == NULL) {
        return;
    }

    const ClientState client = *found;
    if (notifyPeers && client.registered && !client.nick.empty()) {
        std::set<int> peers;
        for (std::map<std::string, Channel>::const_iterator channelIt = _channels.begin(); channelIt != _channels.end(); ++channelIt) {
            if (!channelIt->second.hasMember(fd)) {
                continue;
            }
            const std::vector<int> members = channelIt->second.members();
            for (std::size_t i = 0; i < members.size(); ++i) {
                if (members[i] != fd) {
                    peers.insert(members[i]);
                }
            }
        }
        const std::string quitLine = Replies::formatMessage(prefixFor(client), "QUIT", std::vector<std::string>(1, reason));
        for (std::set<int>::const_iterator peer = peers.begin(); peer != peers.end(); ++peer) {
            sendRaw(*peer, quitLine);
        }
    }

    std::vector<std::string> emptyChannels;
    for (std::map<std::string, Channel>::iterator channelIt = _channels.begin(); channelIt != _channels.end(); ++channelIt) {
        if (channelIt->second.hasMember(fd)) {
            channelIt->second.removeMember(fd);
            if (channelIt->second.empty()) {
                emptyChannels.push_back(channelIt->first);
            }
        }
    }
    for (std::size_t i = 0; i < emptyChannels.size(); ++i) {
        _channels.erase(emptyChannels[i]);
    }

    _clients.erase(fd);
}
