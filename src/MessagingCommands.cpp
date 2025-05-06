#include "IrcApplication.hpp"

#include "IrcMessage.hpp"
#include "Replies.hpp"

#include <vector>

void IrcApplication::handlePrivmsg(int fd, const IrcMessage& message) {
    if (message.params.empty()) {
        sendNumeric(fd, 411, std::vector<std::string>(), "No recipient given (PRIVMSG)");
        return;
    }
    if (message.params.size() < 2 || message.params[1].empty()) {
        sendNumeric(fd, 412, std::vector<std::string>(), "No text to send");
        return;
    }

    const std::vector<std::string> targets = splitComma(message.params[0]);
    for (std::size_t i = 0; i < targets.size(); ++i) {
        const int targetFd = findNick(targets[i]);
        if (targetFd == -1) {
            sendNumeric(fd, 401, std::vector<std::string>(1, targets[i]), "No such nick/channel");
            continue;
        }
        std::vector<std::string> params;
        params.push_back(targets[i]);
        params.push_back(message.params[1]);
        sendRaw(targetFd, Replies::formatMessage(prefixFor(fd), "PRIVMSG", params));
    }
}
