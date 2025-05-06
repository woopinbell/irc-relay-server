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
        const std::string& name = names[i];
        if (!Channel::isValidName(name)) {
            sendNumeric(fd, 403, std::vector<std::string>(1, name), "No such channel");
            continue;
        }

        Channel& channel = ensureChannel(name);
        if (channel.hasMember(fd)) {
            sendNames(fd, channel);
            continue;
        }
        if (channel.isInviteOnly() && !channel.isInvited(_clients.state(fd).nick)) {
            sendNumeric(fd, 473, std::vector<std::string>(1, name), "Cannot join channel (+i)");
            continue;
        }

        const bool firstMember = channel.empty();
        channel.addMember(fd, firstMember);
        channel.clearInvite(_clients.state(fd).nick);

        broadcastToChannel(name, Replies::formatMessage(prefixFor(fd), "JOIN", std::vector<std::string>(1, name)), -1);
        sendTopicReply(fd, channel);
        sendNames(fd, channel);
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

    const std::string targetNick = message.params[1];
    const int targetFd = findNick(targetNick);
    if (targetFd == -1) {
        sendNumeric(fd, 401, std::vector<std::string>(1, targetNick), "No such nick/channel");
        return;
    }
    if (!channel->hasMember(targetFd)) {
        std::vector<std::string> params;
        params.push_back(targetNick);
        params.push_back(channel->name());
        sendNumeric(fd, 441, params, "They aren't on that channel");
        return;
    }

    const std::string comment = message.params.size() > 2 ? message.params[2] : targetNick;
    std::vector<std::string> params;
    params.push_back(channel->name());
    params.push_back(targetNick);
    params.push_back(comment);
    broadcastToChannel(channel->name(), Replies::formatMessage(prefixFor(fd), "KICK", params), -1);
    channel->removeMember(targetFd);
    eraseChannelIfEmpty(channel->name());
}
