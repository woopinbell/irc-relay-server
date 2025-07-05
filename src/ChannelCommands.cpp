#include "IrcApplication.hpp"

#include "IrcMessage.hpp"
#include "Replies.hpp"

#include <set>
#include <vector>

void IrcApplication::handleJoin(int fd, const IrcMessage& message) {
    if (message.params.empty()) {
        sendNumeric(fd, 461, std::vector<std::string>(1, "JOIN"), "Not enough parameters");
        return;
    }
    if (message.params[0] == "0") {
        partAllChannels(fd, "Leaving all channels");
        return;
    }

    const std::vector<std::string> names = splitComma(message.params[0]);
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::string name = names[i];
        if (!Channel::isValidName(name)) {
            if (!sendNumeric(fd, 403, std::vector<std::string>(1, name), "No such channel")) {
                return;
            }
            continue;
        }

        Channel& channel = ensureChannel(name);
        if (channel.hasMember(fd)) {
            if (!sendNames(fd, channel)) {
                return;
            }
            continue;
        }
        const ClientState* client = _clients.find(fd);
        if (client == NULL) {
            return;
        }
        if (channel.isInviteOnly() && !channel.isInvited(client->nick)) {
            if (!sendNumeric(fd, 473, std::vector<std::string>(1, name), "Cannot join channel (+i)")) {
                return;
            }
            continue;
        }

        const bool firstMember = channel.empty();
        const std::string nick = client->nick;
        channel.addMember(fd, firstMember);
        channel.clearInvite(nick);
        broadcastToChannel(name, Replies::formatMessage(prefixFor(fd), "JOIN", std::vector<std::string>(1, name)), -1);
        if (!_clients.contains(fd)) {
            return;
        }
        std::map<std::string, Channel>::iterator current = _channels.find(name);
        if (current == _channels.end() || !sendTopicReply(fd, current->second)) {
            return;
        }
        current = _channels.find(name);
        if (current == _channels.end() || !sendNames(fd, current->second)) {
            return;
        }
    }
}

void IrcApplication::handlePart(int fd, const IrcMessage& message) {
    if (message.params.empty()) {
        sendNumeric(fd, 461, std::vector<std::string>(1, "PART"), "Not enough parameters");
        return;
    }

    const std::string reason = message.params.size() > 1 ? message.params[1] : "";
    const std::vector<std::string> names = splitComma(message.params[0]);
    for (std::size_t i = 0; i < names.size(); ++i) {
        partChannel(fd, names[i], reason);
    }
}

void IrcApplication::handleTopic(int fd, const IrcMessage& message) {
    if (message.params.empty()) {
        sendNumeric(fd, 461, std::vector<std::string>(1, "TOPIC"), "Not enough parameters");
        return;
    }

    Channel* channel = findChannelForCommand(fd, message.params[0], true);
    if (!channel) {
        return;
    }

    if (message.params.size() == 1) {
        sendTopicReply(fd, *channel);
        return;
    }

    if (channel->isTopicProtected() && !channel->isOperator(fd)) {
        sendNumeric(fd, 482, std::vector<std::string>(1, channel->name()), "You're not channel operator");
        return;
    }

    channel->setTopic(message.params[1]);
    std::vector<std::string> params;
    params.push_back(channel->name());
    params.push_back(message.params[1]);
    broadcastToChannel(channel->name(), Replies::formatMessage(prefixFor(fd), "TOPIC", params), -1);
}

void IrcApplication::handleKick(int fd, const IrcMessage& message) {
    if (message.params.size() < 2) {
        sendNumeric(fd, 461, std::vector<std::string>(1, "KICK"), "Not enough parameters");
        return;
    }
    Channel* channel = findChannelForCommand(fd, message.params[0], true);
    if (!channel) {
        return;
    }
    if (!channel->isOperator(fd)) {
        sendNumeric(fd, 482, std::vector<std::string>(1, channel->name()), "You're not channel operator");
        return;
    }
    const std::string channelName = channel->name();
    const std::string targetNick = message.params[1];
    const int targetFd = findNick(targetNick);
    if (targetFd == -1) {
        sendNumeric(fd, 401, std::vector<std::string>(1, targetNick), "No such nick/channel");
        return;
    }
    if (!channel->hasMember(targetFd)) {
        std::vector<std::string> params;
        params.push_back(targetNick);
        params.push_back(channelName);
        sendNumeric(fd, 441, params, "They aren't on that channel");
        return;
    }
    const std::string comment = message.params.size() > 2 ? message.params[2] : targetNick;
    std::vector<std::string> params;
    params.push_back(channelName);
    params.push_back(targetNick);
    params.push_back(comment);
    broadcastToChannel(channelName, Replies::formatMessage(prefixFor(fd), "KICK", params), -1);
    std::map<std::string, Channel>::iterator current = _channels.find(channelName);
    if (current == _channels.end()) {
        return;
    }
    current->second.removeMember(targetFd);
    eraseChannelIfEmpty(channelName);
}

void IrcApplication::handleInvite(int fd, const IrcMessage& message) {
    if (message.params.size() < 2) {
        sendNumeric(fd, 461, std::vector<std::string>(1, "INVITE"), "Not enough parameters");
        return;
    }
    const std::string targetNick = message.params[0];
    const std::string channelName = message.params[1];
    const int targetFd = findNick(targetNick);
    if (targetFd == -1) {
        sendNumeric(fd, 401, std::vector<std::string>(1, targetNick), "No such nick/channel");
        return;
    }
    Channel* channel = findChannelForCommand(fd, channelName, true);
    if (!channel) {
        return;
    }
    if (channel->isInviteOnly() && !channel->isOperator(fd)) {
        sendNumeric(fd, 482, std::vector<std::string>(1, channelName), "You're not channel operator");
        return;
    }
    if (channel->hasMember(targetFd)) {
        std::vector<std::string> params;
        params.push_back(targetNick);
        params.push_back(channelName);
        sendNumeric(fd, 443, params, "is already on channel");
        return;
    }
    channel->invite(targetNick);
    const std::string sourcePrefix = prefixFor(fd);
    std::vector<std::string> invitingParams;
    invitingParams.push_back(replyTarget(fd));
    invitingParams.push_back(targetNick);
    invitingParams.push_back(channelName);
    const std::string acknowledgement = Replies::formatMessage(_serverName, "341", invitingParams);
    std::vector<std::string> inviteParams;
    inviteParams.push_back(targetNick);
    inviteParams.push_back(channelName);
    const std::string invitation = Replies::formatMessage(sourcePrefix, "INVITE", inviteParams);
    sendRaw(fd, acknowledgement);
    sendRaw(targetFd, invitation);
}

void IrcApplication::handleMode(int fd, const IrcMessage& message) {
    if (message.params.empty()) {
        sendNumeric(fd, 461, std::vector<std::string>(1, "MODE"), "Not enough parameters");
        return;
    }

    const std::string target = message.params[0];
    if (isChannelTarget(target)) {
        handleChannelMode(fd, message);
        return;
    }

    const int targetFd = findNick(target);
    if (targetFd == -1) {
        sendNumeric(fd, 401, std::vector<std::string>(1, target), "No such nick/channel");
        return;
    }
    if (message.params.size() == 1) {
        sendNumericRaw(fd, 221, std::vector<std::string>(1, "+"));
        return;
    }
    if (targetFd != fd) {
        sendNumeric(fd, 502, std::vector<std::string>(), "Cannot change mode for other users");
        return;
    }
    sendNumeric(fd, 501, std::vector<std::string>(), "User modes are not implemented");
}

void IrcApplication::handleList(int fd, const IrcMessage& message) {
    std::set<std::string> requested;
    if (!message.params.empty()) {
        const std::vector<std::string> names = splitComma(message.params[0]);
        requested.insert(names.begin(), names.end());
    }

    sendNumericRaw(fd, 321, std::vector<std::string>{"Channel", "Users", "Name"});
    for (std::map<std::string, Channel>::const_iterator it = _channels.begin(); it != _channels.end(); ++it) {
        if (!requested.empty() && requested.find(it->first) == requested.end()) {
            continue;
        }
        std::vector<std::string> params;
        params.push_back(it->first);
        params.push_back(std::to_string(it->second.memberCount()));
        sendNumeric(fd, 322, params, it->second.hasTopic() ? it->second.topic() : "open room");
    }
    sendNumeric(fd, 323, std::vector<std::string>(), "End of /LIST");
}

void IrcApplication::handleNames(int fd, const IrcMessage& message) {
    if (message.params.empty()) {
        for (std::map<std::string, Channel>::const_iterator it = _channels.begin(); it != _channels.end(); ++it) {
            sendNames(fd, it->second);
        }
        return;
    }

    const std::vector<std::string> names = splitComma(message.params[0]);
    for (std::size_t i = 0; i < names.size(); ++i) {
        std::map<std::string, Channel>::const_iterator found = _channels.find(names[i]);
        if (found != _channels.end()) {
            sendNames(fd, found->second);
        } else {
            sendNumeric(fd, 366, std::vector<std::string>(1, names[i]), "End of /NAMES list");
        }
    }
}

void IrcApplication::handleChannelMode(int fd, const IrcMessage& message) {
    Channel* channel = findChannelForCommand(fd, message.params[0], false);
    if (!channel) {
        return;
    }

    if (message.params.size() == 1) {
        std::vector<std::string> params;
        params.push_back(channel->name());
        params.push_back(channel->modeString());
        sendNumericRaw(fd, 324, params);
        return;
    }
    if (!channel->hasMember(fd)) {
        sendNumeric(fd, 442, std::vector<std::string>(1, channel->name()), "You're not on that channel");
        return;
    }
    if (!channel->isOperator(fd)) {
        sendNumeric(fd, 482, std::vector<std::string>(1, channel->name()), "You're not channel operator");
        return;
    }

    bool adding = true;
    std::size_t argIndex = 2;
    const std::string modes = message.params[1];
    for (std::size_t i = 0; i < modes.size(); ++i) {
        const char mode = modes[i];
        if (mode == '+') {
            adding = true;
            continue;
        }
        if (mode == '-') {
            adding = false;
            continue;
        }

        if (mode == 'i') {
            channel->setInviteOnly(adding);
            broadcastMode(fd, *channel, std::string(adding ? "+" : "-") + "i", "");
        } else if (mode == 't') {
            channel->setTopicProtected(adding);
            broadcastMode(fd, *channel, std::string(adding ? "+" : "-") + "t", "");
        } else if (mode == 'o') {
            if (argIndex >= message.params.size()) {
                sendNumeric(fd, 461, std::vector<std::string>(1, "MODE"), "Not enough parameters");
                continue;
            }
            const std::string nick = message.params[argIndex++];
            const int targetFd = findNick(nick);
            if (targetFd == -1 || !channel->hasMember(targetFd)) {
                std::vector<std::string> params;
                params.push_back(nick);
                params.push_back(channel->name());
                sendNumeric(fd, 441, params, "They aren't on that channel");
                continue;
            }
            channel->setOperator(targetFd, adding);
            broadcastMode(fd, *channel, std::string(adding ? "+" : "-") + "o", _clients.state(targetFd).nick);
        } else {
            sendNumeric(fd, 472, std::vector<std::string>(1, std::string(1, mode)), "is unknown mode char to me");
        }
    }
}
