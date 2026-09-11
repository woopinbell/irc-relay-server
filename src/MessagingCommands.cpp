#include "IrcApplication.hpp"

#include "IrcMessage.hpp"
#include "Replies.hpp"

#include <vector>

// [INTV:ARCH] 채널 대상과 개인 대상을 target[0] == '#'/'&' 여부(isChannelTarget)로 분기 — 채널은
// broadcastToChannel(발신자 제외)로, 개인은 findNick 후 단일 sendRaw로 각각 다른 라우팅 경로를 탄다.
// 콤마로 여러 대상을 한 번에 지정할 수 있어(splitComma) 대상별로 실패해도 나머지 대상 처리는 계속된다.
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
        const std::string& target = targets[i];
        if (isChannelTarget(target)) {
            std::map<std::string, Channel>::const_iterator channelIt = _channels.find(target);
            if (channelIt == _channels.end() || !channelIt->second.hasMember(fd)) {
                sendNumeric(fd, 404, std::vector<std::string>(1, target), "Cannot send to channel");
                continue;
            }
            std::vector<std::string> params;
            params.push_back(target);
            params.push_back(message.params[1]);
            broadcastToChannel(target, Replies::formatMessage(prefixFor(fd), "PRIVMSG", params), fd);
            ++_metrics.messagesRelayed;
        } else {
            const int targetFd = findNick(target);
            if (targetFd == -1) {
                sendNumeric(fd, 401, std::vector<std::string>(1, target), "No such nick/channel");
                continue;
            }
            std::vector<std::string> params;
            params.push_back(target);
            params.push_back(message.params[1]);
            sendRaw(targetFd, Replies::formatMessage(prefixFor(fd), "PRIVMSG", params));
            ++_metrics.messagesRelayed;
        }
    }
}
