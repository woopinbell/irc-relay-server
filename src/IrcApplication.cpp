#include "IrcApplication.hpp"

#include "Connection.hpp"
#include "IrcMessage.hpp"
#include "Replies.hpp"

#include <chrono>

IrcApplication::IrcApplication(Server& server, const std::string& password, const RuntimeConfig& runtime)
    : _server(server),
      _password(password),
      _runtime(runtime),
      _serverName("irc.relay.local"),
      _nextHeartbeatToken(0) {
}

void IrcApplication::onConnect(Connection& connection) {
    const MonotonicTime now = MonotonicClock::now();
    ClientState client;
    client.fd = connection.fd();
    client.host = connection.peerAddress();
    client.passOk = _password.empty();
    client.connectedAt = now;
    client.lastActivityAt = now;
    _clients.state(client.fd) = client;
    logEvent("client_connected", std::vector<std::pair<std::string, std::string> >{
        std::make_pair("fd", std::to_string(client.fd)),
        std::make_pair("peer", client.host)
    });
}

// [INTV:EDGE] !_clients.contains(fd)일 때 onConnect를 다시 호출하는 방어적 분기 — 정상 경로라면
// Server가 onConnect를 먼저 호출해 상태가 이미 있어야 하지만, 테스트 하네스가 onLine을 직접 호출하는
// 경우 등 이 불변조건이 깨진 상태에서도 크래시 대신 상태를 만들어 계속 진행하게 한다.
void IrcApplication::onLine(Connection& connection, const std::string& line) {
    const int fd = connection.fd();
    if (!_clients.contains(fd)) {
        onConnect(connection);
    }
    const MonotonicTime now = MonotonicClock::now();
    _clients.state(fd).lastActivityAt = now;

    IrcMessage message;
    std::string parseError;
    if (!IrcMessage::parseLine(line, message, &parseError)) {
        sendNumeric(fd, 417, std::vector<std::string>(), parseError);
        return;
    }
    if (!recordCommand(fd, now)) {
        return;
    }
    ++_metrics.commandsHandled;
    handleMessage(fd, message);
}

void IrcApplication::onDisconnect(Connection& connection, const std::string& reason) {
    removeClientState(connection.fd(), reason, true);
    logEvent("client_disconnected", std::vector<std::pair<std::string, std::string> >{
        std::make_pair("fd", std::to_string(connection.fd())),
        std::make_pair("reason", reason)
    });
}

void IrcApplication::onTick() {
    const MonotonicTime now = MonotonicClock::now();
    const std::vector<int> fds = _clients.fds();
    for (std::size_t i = 0; i < fds.size(); ++i) {
        maintainClient(fds[i], now);
    }
}

void IrcApplication::shutdown(const std::string& reason) {
    const std::vector<int> fds = _clients.fds();
    for (std::size_t i = 0; i < fds.size(); ++i) {
        sendRaw(fds[i], Replies::error(reason));
        requestClose(fds[i], reason);
    }
    logMetrics();
}

void IrcApplication::logMetrics() const {
    const Server::Metrics& serverMetrics = _server.metrics();
    logEvent("server_metrics", std::vector<std::pair<std::string, std::string> >{
        std::make_pair("accepted", std::to_string(serverMetrics.acceptedConnections)),
        std::make_pair("closed", std::to_string(serverMetrics.closedConnections)),
        std::make_pair("lines", std::to_string(serverMetrics.linesReceived)),
        std::make_pair("queue_drops", std::to_string(serverMetrics.outboundQueueDrops)),
        std::make_pair("commands", std::to_string(_metrics.commandsHandled)),
        std::make_pair("messages", std::to_string(_metrics.messagesRelayed)),
        std::make_pair("rooms", std::to_string(_channels.size())),
        std::make_pair("rooms_created", std::to_string(_metrics.roomsCreated)),
        std::make_pair("rate_limited", std::to_string(_metrics.rateLimitedClients)),
        std::make_pair("idle_timeouts", std::to_string(_metrics.idleTimeouts)),
        std::make_pair("heartbeats", std::to_string(_metrics.heartbeatPings))
    });
}

// [INTV:ARCH] if-else 체인으로 된 명령 디스패치 테이블 — PASS/NICK/USER/PING/PONG/QUIT은 등록
// (registration) 전에도 허용되고, 그 외 명령은 등록되지 않은 클라이언트에게 451로 즉시 거부된다.
// - [TRAP] 이 순서(등록 여부 체크가 PASS~QUIT 분기들 "다음"에 옴)를 지키지 않고 등록 체크를 맨 앞으로
//   옮기면, 애초에 등록을 진행해야 할 PASS/NICK/USER 명령 자체가 "등록 안 됐다"는 이유로 막혀버려
//   아무도 등록할 수 없는 교착 상태가 된다.
void IrcApplication::handleMessage(int fd, const IrcMessage& message) {
    if (message.command == "PASS") {
        handlePass(fd, message);
    } else if (message.command == "NICK") {
        handleNick(fd, message);
    } else if (message.command == "USER") {
        handleUser(fd, message);
    } else if (message.command == "PING") {
        handlePing(fd, message);
    } else if (message.command == "PONG") {
        handlePong(fd, message);
    } else if (message.command == "QUIT") {
        handleQuit(fd, message);
    } else if (!_clients.state(fd).registered) {
        sendNumeric(fd, 451, std::vector<std::string>(), "You have not registered");
    } else if (message.command == "PRIVMSG") {
        handlePrivmsg(fd, message);
    } else if (message.command == "JOIN") {
        handleJoin(fd, message);
    } else if (message.command == "PART") {
        handlePart(fd, message);
    } else if (message.command == "TOPIC") {
        handleTopic(fd, message);
    } else if (message.command == "KICK") {
        handleKick(fd, message);
    } else if (message.command == "INVITE") {
        handleInvite(fd, message);
    } else if (message.command == "MODE") {
        handleMode(fd, message);
    } else if (message.command == "LIST") {
        handleList(fd, message);
    } else if (message.command == "NAMES") {
        handleNames(fd, message);
    } else if (message.command == "METRICS") {
        handleMetrics(fd);
    } else {
        sendNumeric(fd, 421, std::vector<std::string>(1, message.command), "Unknown command");
    }
}

// [INTV:FLOW] 클라이언트당 상태 기계 3단계를 매 틱(onTick) 확인: 1) 미등록 상태로 registrationTimeout을
// 넘기면 강제 종료 -> 2) PONG을 기다리는 중인데 pingTimeout을 넘기면(무응답) 강제 종료 -> 3) 비활성
// 상태가 idleTimeout을 넘기면 PING을 보내고 PONG 대기 상태로 전환.
// - [TRAP] awaitingPong 플래그 없이 "마지막 활동 시각만" 보고 반복적으로 PING을 계속 보내도록
//   재구현하면, 이미 보낸 PING에 대한 응답을 기다리는 중에 또 PING을 보내 상태가 헷갈리게 된다.
//   "PING을 보냈으면 PONG이 올 때까지는 그 PING의 결과만 판정한다"는 상태 분리가 핵심.
void IrcApplication::maintainClient(int fd, const MonotonicTime& now) {
    ClientState* client = _clients.find(fd);
    if (client == NULL) {
        return;
    }
    if (!client->registered &&
        now - client->connectedAt >= std::chrono::seconds(_runtime.registrationTimeoutSeconds)) {
        sendNumeric(fd, 451, std::vector<std::string>(), "Registration timeout");
        requestClose(fd, "registration timeout");
        return;
    }
    if (_runtime.idleTimeoutSeconds <= 0) {
        return;
    }
    if (client->awaitingPong &&
        now - client->lastPingAt >= std::chrono::seconds(_runtime.pingTimeoutSeconds)) {
        ++_metrics.idleTimeouts;
        sendRaw(fd, Replies::error("Ping timeout"));
        requestClose(fd, "ping timeout");
        logEvent("client_ping_timeout", std::vector<std::pair<std::string, std::string> >{
            std::make_pair("fd", std::to_string(fd)),
            std::make_pair("nick", replyTarget(fd))
        });
        return;
    }
    if (!client->awaitingPong &&
        now - client->lastActivityAt >= std::chrono::seconds(_runtime.idleTimeoutSeconds)) {
        const std::string token =
            "heartbeat-" + std::to_string(fd) + "-" + std::to_string(++_nextHeartbeatToken);
        client->awaitingPong = true;
        client->pendingPongToken = token;
        client->lastPingAt = now;
        if (!sendRaw(fd, Replies::formatMessage(_serverName, "PING", std::vector<std::string>(1, token)))) {
            return;
        }
        ++_metrics.heartbeatPings;
    }
}

// [INTV:PERF] [INTV:EDGE] 슬라이딩 윈도우 레이트 리밋: commandWindow 앞쪽에서 윈도우(rateLimitWindow
// Seconds)보다 오래된 타임스탬프를 pop한 뒤 이번 명령을 push하고, 남은 개수가 한도를 넘으면 거부한다.
// - [FLOW] 1. 윈도우 밖으로 나간 오래된 기록을 앞에서부터 제거 -> 2. 현재 명령 시각을 추가 -> 3. 남은
//   개수와 한도를 비교해 초과 시 즉시 연결 종료(요청 거부가 아니라 연결 자체를 끊는 강한 정책)
// - [TRAP] "제거 -> 추가" 순서를 바꿔 먼저 push하고 나중에 trim하면, 트림 조건(>= window) 판정 시점에
//   방금 추가한 현재 명령까지 포함해 비교하게 되어 경계값에서 오프바이원 오차가 생길 수 있다.
bool IrcApplication::recordCommand(int fd, const MonotonicTime& now) {
    ClientState& client = _clients.state(fd);
    while (!client.commandWindow.empty() &&
           now - client.commandWindow.front() >=
               std::chrono::seconds(_runtime.rateLimitWindowSeconds)) {
        client.commandWindow.pop_front();
    }
    client.commandWindow.push_back(now);
    if (_runtime.rateLimitCount != 0 && client.commandWindow.size() > _runtime.rateLimitCount) {
        ++_metrics.rateLimitedClients;
        sendNumeric(fd, 439, std::vector<std::string>(), "Command rate limit exceeded");
        requestClose(fd, "command rate limit exceeded");
        logEvent("client_rate_limited", std::vector<std::pair<std::string, std::string> >{
            std::make_pair("fd", std::to_string(fd)),
            std::make_pair("nick", replyTarget(fd))
        });
        return false;
    }
    return true;
}
