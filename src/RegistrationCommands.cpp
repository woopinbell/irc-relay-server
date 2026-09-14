#include "IrcApplication.hpp"

#include "IrcMessage.hpp"
#include "Replies.hpp"

#include <cctype>
#include <utility>
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

// [INTV:ARCH] 등록(registration) 핸드셰이크는 PASS/NICK/USER 세 명령이 각자 독립된 순서로 도착할 수
//있는 3-게이트 구조 — maybeRegister()가 매번 (passOk && hasNick && hasUser) 전부를 확인해, 셋 중
// 마지막으로 도착한 명령이 자연스럽게 등록을 완료시킨다. 클라이언트가 어떤 순서로 보내든 동작해야
// 하는 IRC 프로토콜 요구사항을 상태 플래그 조합으로 처리한 것.
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

    // [INTV:EDGE] collision != fd 체크: 자기 자신의 fd가 이미 그 닉네임의 소유자로 인덱싱되어 있는
    // 경우(예: 대소문자만 바꿔 같은 닉네임을 다시 보낸 경우)까지 "충돌"로 오판하지 않기 위함이다.
    const int collision = _clients.findFdByNickname(nextNick);
    if (collision != -1 && collision != fd) {
        sendNumeric(fd, 433, std::vector<std::string>(1, nextNick), "Nickname is already in use");
        return;
    }

    ClientState& client = _clients.state(fd);
    const bool wasRegistered = client.registered;
    // [INTV:TRAP] 개명 전(옛 닉네임 기준)의 prefix를 미리 캡처해둬야 한다 — setNickname() 이후에
    // prefixFor()를 부르면 이미 새 닉네임이 반영되어, "누가 개명했는지"를 알리는 NICK 브로드캐스트의
    // 발신자 표시(oldnick!user@host)가 새 닉네임으로 잘못 나가게 된다.
    const std::string oldPrefix = prefixFor(client);

    _clients.setNickname(fd, nextNick);

    if (wasRegistered) {
        broadcastToCommon(fd, Replies::formatMessage(oldPrefix, "NICK", std::vector<std::string>(1, nextNick)), true);
        // [INTV:EDGE] broadcastToCommon이 내부적으로 sendTo를 호출하는데, 그 과정에서 송신 실패로
        // 이 fd 자신의 연결이 끊겼을 수 있다 — 이후 maybeRegister(fd)를 안전하게 부를 수 있는지
        // 다시 확인.
        if (!_clients.contains(fd)) {
            return;
        }
    }

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

void IrcApplication::handlePing(int fd, const IrcMessage& message) {
    if (message.params.empty()) {
        sendNumeric(fd, 409, std::vector<std::string>(), "No origin specified");
        return;
    }
    std::vector<std::string> params;
    params.push_back(_serverName);
    params.push_back(message.params[0]);
    sendRaw(fd, Replies::formatMessage(_serverName, "PONG", params));
}

// [INTV:EDGE] pendingPongToken과 정확히 일치하는 토큰만 유효한 PONG으로 인정 — 클라이언트가 보낸
// 임의의 PONG(예전 하트비트에 대한 지연 응답, 위조된 응답)이 지금 진행 중인 타임아웃 판정을 조작하지
// 못하게 막는 토큰 검증.
void IrcApplication::handlePong(int fd, const IrcMessage& message) {
    ClientState& client = _clients.state(fd);
    if (!client.awaitingPong ||
        message.params.size() != 1 ||
        message.params[0] != client.pendingPongToken) {
        return;
    }
    client.awaitingPong = false;
    client.pendingPongToken.clear();
    client.lastPingAt = MonotonicTime();
}

void IrcApplication::handleQuit(int fd, const IrcMessage& message) {
    const std::string reason = message.params.empty() ? "Client Quit" : message.params[0];
    requestClose(fd, reason);
}

// [INTV:ARCH] 등록 3-게이트(passOk/hasNick/hasUser)가 전부 충족됐을 때만 웰컴 메시지를 보내는 관문
// 함수 — handlePass/handleNick/handleUser 세 곳 모두에서 호출되지만, 실제로 등록이 완료되는 시점은
// "마지막으로 도착한 그 한 번"뿐이다(registered 플래그가 중복 실행을 막는다).
void IrcApplication::maybeRegister(int fd) {
    ClientState* client = _clients.find(fd);
    if (client == NULL || client->registered || !client->passOk || !client->hasNick || !client->hasUser) {
        return;
    }
    client->registered = true;
    const std::string nick = client->nick;
    if (!sendNumeric(fd, 1, std::vector<std::string>(), "Welcome to irc-relay-server, " + nick)) {
        return;
    }
    if (!sendNumeric(fd, 2, std::vector<std::string>(), "Your host is " + _serverName)) {
        return;
    }
    if (!sendNumeric(fd, 3, std::vector<std::string>(), "This server is running a C++17 event backend")) {
        return;
    }
    logEvent("client_registered", std::vector<std::pair<std::string, std::string> >{
        std::make_pair("fd", std::to_string(fd)),
        std::make_pair("nick", nick)
    });
}
