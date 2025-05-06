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
