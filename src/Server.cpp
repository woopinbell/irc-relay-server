#include "Server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace irc {
namespace {

// [INTV:EDGE] fcntl(F_GETFD/F_SETFD, FD_CLOEXEC): 이 fd가 fork+exec로 실행되는 자식 프로세스에
// 상속되지 않게 막는 close-on-exec 설정 — fd 누수 방지.
void setCloseOnExec(int fd)
{
    const int flags = ::fcntl(fd, F_GETFD, 0);
    if (flags == -1) {
        throw std::system_error(errno, std::generic_category(), "fcntl F_GETFD");
    }
    if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) {
        throw std::system_error(errno, std::generic_category(), "fcntl F_SETFD");
    }
}

// [INTV:ARCH] O_NONBLOCK: recv/send/accept가 즉시 처리할 데이터가 없어도 블로킹하지 않고 EAGAIN으로
// 바로 리턴하게 만든다 — 이벤트 루프(리액터) 모델의 전제 조건.
// - [TRAP] 이 설정을 빼먹고 블로킹 소켓으로 재구현하면, 한 클라이언트를 기다리는 동안 이벤트 루프
//   자체가 멈춰 다른 모든 연결의 처리가 정지된다.
void setNonBlocking(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        throw std::system_error(errno, std::generic_category(), "fcntl F_GETFL");
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        throw std::system_error(errno, std::generic_category(), "fcntl F_SETFL");
    }
}

// [INTV:TRADE_OFF] 상대가 이미 끊은 소켓에 send()하면 기본적으로 SIGPIPE로 프로세스가 죽을 수 있다.
// SO_NOSIGPIPE(BSD/macOS 전용)를 켜서 send()가 대신 EPIPE 에러를 리턴하게 만든다 — 리눅스는 이 옵션이
// 없는 대신 send 호출마다 MSG_NOSIGNAL 플래그로 같은 효과를 낸다(Connection.cpp 참고, 플랫폼별 대응).
void setNoSigPipe(int fd)
{
#ifdef SO_NOSIGPIPE
    const int enabled = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) == -1) {
        throw std::system_error(errno, std::generic_category(), "setsockopt SO_NOSIGPIPE");
    }
#else
    (void)fd;
#endif
}

// sockaddr_storage: IPv4/IPv6 주소를 모두 담을 수 있는 범용 크기의 구조체 — ss_family로 실제 담긴
// 주소 체계를 보고 해당 타입으로 캐스팅해 해석한다.
std::string formatPeerAddress(const sockaddr_storage& storage)
{
    char address[INET6_ADDRSTRLEN];

    if (storage.ss_family == AF_INET) {
        const sockaddr_in* ipv4 = reinterpret_cast<const sockaddr_in*>(&storage);
        if (::inet_ntop(AF_INET, &ipv4->sin_addr, address, sizeof(address)) == NULL) {
            return "unknown";
        }
        return std::string(address) + ":" + std::to_string(ntohs(ipv4->sin_port));
    }
    if (storage.ss_family == AF_INET6) {
        const sockaddr_in6* ipv6 = reinterpret_cast<const sockaddr_in6*>(&storage);
        if (::inet_ntop(AF_INET6, &ipv6->sin6_addr, address, sizeof(address)) == NULL) {
            return "unknown";
        }
        return std::string("[") + address + "]:" + std::to_string(ntohs(ipv6->sin6_port));
    }
    return "unknown";
}

std::string eventErrorMessage(const Event& event)
{
    if (event.errorCode == 0) {
        return "socket readiness error";
    }
    return std::string("socket readiness error: ") + std::strerror(event.errorCode);
}

} // namespace

Server::Server(Config config)
    : config_(std::move(config))
    , listenFd_(-1)
    , running_(false)
    , stopRequested_(false)
{
}

Server::Server(Config config, std::unique_ptr<EventManager> eventManager)
    : config_(std::move(config))
    , listenFd_(-1)
    , eventManager_(std::move(eventManager))
    , running_(false)
    , stopRequested_(false)
{
    // [INTV:EDGE] 테스트 등에서 커스텀 EventManager를 주입할 수 있지만, null을 넣는 실수는 여기서
    // 바로 걸러 나중에 엉뚱한 곳(pollOnce 등)에서 널 포인터 역참조로 튀지 않게 한다 — 조기 검증(fail fast).
    if (!eventManager_) {
        throw std::invalid_argument("event manager must not be null");
    }
}

Server::~Server()
{
    stop();
    closeAllConnections();
    closeListenSocket();
}

void Server::start()
{
    if (running_) {
        return;
    }

    // [INTV:ARCH] 생성자에서 EventManager를 안 받았다면 이 시점에 플랫폼 기본 구현(epoll/kqueue)을
    // 만든다 — 지연 초기화(lazy init).
    if (!eventManager_) {
        eventManager_ = EventManager::createDefault();
    }
    createListenSocket();
    try {
        eventManager_->addFd(listenFd_, EventInterest::Read);
    } catch (...) {
        // [INTV:EDGE] 리스닝 소켓 등록이 실패하면 이미 만들어둔 소켓/이벤트매니저를 되돌려(rollback)
        // 절반만 초기화된 상태로 남지 않게 한 뒤 예외를 그대로 다시 던진다.
        closeListenSocket();
        eventManager_.reset();
        throw;
    }
    stopRequested_ = false;
    running_ = true;
}

// [INTV:ARCH] 리액터 패턴의 구동부: stop()이 호출되기 전까지 "이벤트 대기 -> 처리"를 반복한다.
void Server::run()
{
    if (!running_) {
        start();
    }

    while (running_ && !stopRequested_) {
        pollOnce(config_.eventTimeoutMs);
    }

    running_ = false;
    closeAllConnections();
    closeListenSocket();
    eventManager_.reset();
}

// [INTV:ARCH] 루프 한 바퀴(한 틱): wait로 이벤트를 받아 리스닝 소켓이면 accept, 그 외 fd면 클라이언트
// 이벤트로 처리한다. run()이 이걸 반복 호출하지만, 테스트나 다른 이벤트 루프에 통합하려면 이 메서드를
// 직접 반복 호출해도 된다.
void Server::pollOnce(int timeoutMs)
{
    if (!running_) {
        throw std::logic_error("Server::pollOnce called before start");
    }

    const int effectiveTimeout = timeoutMs < 0 ? config_.eventTimeoutMs : timeoutMs;
    const std::vector<Event> events = eventManager_->wait(effectiveTimeout);

    for (std::vector<Event>::const_iterator it = events.begin(); it != events.end(); ++it) {
        // [INTV:EDGE] 이번 배치를 처리하던 도중 어떤 핸들러가 stop()을 호출했다면, 남은 이벤트는
        // 처리하지 않고 이번 pollOnce를 즉시 끝낸다 — 콜백이 서버를 멈춘 뒤에도 계속 이벤트를
        // 처리하면 "멈췄는데 계속 돈다"는 모순된 상태가 된다.
        if (stopRequested_) {
            break;
        }
        if (it->fd == listenFd_) {
            if (it->error) {
                throw std::runtime_error(eventErrorMessage(*it));
            }
            if (hasInterest(it->interests, EventInterest::Read)) {
                acceptReadyClients();
            }
            continue;
        }
        handleClientEvent(*it);
    }
}

void Server::stop() noexcept
{
    stopRequested_ = true;
    running_ = false;
}

bool Server::isRunning() const noexcept
{
    return running_;
}

const Server::Config& Server::config() const noexcept
{
    return config_;
}

int Server::listenFd() const noexcept
{
    return listenFd_;
}

std::uint16_t Server::port() const noexcept
{
    return config_.port;
}

std::size_t Server::connectionCount() const noexcept
{
    return connections_.size();
}

const Server::Metrics& Server::metrics() const noexcept
{
    return metrics_;
}

void Server::setConnectHandler(ConnectHandler handler)
{
    onConnect_ = std::move(handler);
}

void Server::setLineHandler(LineHandler handler)
{
    onLine_ = std::move(handler);
}

void Server::setDisconnectHandler(DisconnectHandler handler)
{
    onDisconnect_ = std::move(handler);
}

void Server::setErrorHandler(ErrorHandler handler)
{
    onError_ = std::move(handler);
}

// [INTV:ARCH] 소켓에 바로 send()하지 않고 Connection의 송신 버퍼에 큐잉만 해두는 이유: 논블로킹
// 소켓은 한 번에 전부 못 보낼 수 있어, 실제 전송은 이 fd가 "쓰기 가능" 이벤트를 받을 때
// flushPending으로 처리한다. 큐잉 후 refreshInterest를 호출해 이벤트 매니저가 Write 이벤트도
// 통지하도록 관심사를 갱신한다.
bool Server::sendTo(int fd, const std::string& line)
{
    Connection* connection = findConnection(fd);
    if (connection == NULL) {
        return false;
    }
    const bool queued = connection->queueLine(line);
    if (!queued) {
        ++metrics_.outboundQueueDrops;
    }
    const bool refreshed = refreshInterest(fd);
    return queued && refreshed;
}

bool Server::queueRawTo(int fd, const std::string& bytes)
{
    Connection* connection = findConnection(fd);
    if (connection == NULL) {
        return false;
    }
    const bool queued = connection->queueRaw(bytes);
    if (!queued) {
        ++metrics_.outboundQueueDrops;
    }
    const bool refreshed = refreshInterest(fd);
    return queued && refreshed;
}

// [INTV:EDGE] erase 전에 소유권을 로컬 unique_ptr로 옮겨(move) 둔다 — 그래야 connections_에서 이미
// 제거된 뒤에도 아래 onDisconnect_ 콜백에 살아있는 Connection 레퍼런스를 넘겨줄 수 있고, 콜백 안에서
// findConnection(fd)를 호출하면 "이미 없다"는 정확한 상태를 보게 된다.
// - [TRAP] erase를 먼저 하고 found->second로 콜백을 부르면 이미 소멸된 객체를 참조하는 use-after-free가
//   된다. 반드시 "옮기기 -> erase -> 옮겨둔 것으로 콜백" 순서를 지킬 것.
void Server::disconnect(int fd, const std::string& reason)
{
    std::unordered_map<int, std::unique_ptr<Connection> >::iterator found = connections_.find(fd);
    if (found == connections_.end()) {
        return;
    }

    if (eventManager_) {
        try {
            eventManager_->removeFd(fd);
        } catch (const std::exception& exception) {
            reportError(exception.what());
        }
    }

    std::unique_ptr<Connection> connection = std::move(found->second);
    connections_.erase(found);
    ++metrics_.closedConnections;

    if (onDisconnect_) {
        try {
            onDisconnect_(*connection, reason);
        } catch (const std::exception& exception) {
            reportError(exception.what());
        }
    }
}

Connection* Server::findConnection(int fd)
{
    std::unordered_map<int, std::unique_ptr<Connection> >::iterator found = connections_.find(fd);
    if (found == connections_.end()) {
        return NULL;
    }
    return found->second.get();
}

const Connection* Server::findConnection(int fd) const
{
    std::unordered_map<int, std::unique_ptr<Connection> >::const_iterator found =
        connections_.find(fd);
    if (found == connections_.end()) {
        return NULL;
    }
    return found->second.get();
}

// [INTV:EDGE] 소켓 생성부터 listen까지의 TCP 서버 부트스트랩 시퀀스(socket -> setsockopt -> bind ->
// listen). 중간에 실패하면 catch(...)에서 만들어둔 fd를 닫고 예외를 다시 던져 fd 누수를 막는다
// (성공했을 때만 마지막에 listenFd_에 대입 — 실패 도중엔 멤버 상태를 건드리지 않는다).
void Server::createListenSocket()
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        throw std::system_error(errno, std::generic_category(), "socket");
    }

    try {
        setCloseOnExec(fd);
        setNonBlocking(fd);

        // [INTV:ARCH] SO_REUSEADDR: 서버를 재시작할 때 직전 프로세스가 쓰던 포트가 TIME_WAIT 상태로
        // 남아있어도 즉시 재바인딩할 수 있게 해주는 표준 관용구.
        const int enabled = 1;
        if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) == -1) {
            throw std::system_error(errno, std::generic_category(), "setsockopt SO_REUSEADDR");
        }
        setNoSigPipe(fd);

        sockaddr_in address;
        std::memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_port = htons(config_.port);

        const std::string bindAddress =
            config_.bindAddress.empty() ? std::string("0.0.0.0") : config_.bindAddress;
        if (::inet_pton(AF_INET, bindAddress.c_str(), &address.sin_addr) != 1) {
            throw std::invalid_argument("bindAddress must be an IPv4 address");
        }

        if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1) {
            throw std::system_error(errno, std::generic_category(), "bind");
        }
        if (::listen(fd, config_.backlog) == -1) {
            throw std::system_error(errno, std::generic_category(), "listen");
        }

        // [INTV:ARCH] port를 0으로 설정하면 OS가 비어있는 포트를 임의로 골라 바인딩한다(테스트에서
        // 흔히 씀). getsockname으로 실제 배정된 포트 번호를 다시 읽어와 config_에 반영한다.
        if (config_.port == 0) {
            sockaddr_in boundAddress;
            socklen_t length = sizeof(boundAddress);
            if (::getsockname(fd, reinterpret_cast<sockaddr*>(&boundAddress), &length) == -1) {
                throw std::system_error(errno, std::generic_category(), "getsockname");
            }
            config_.port = ntohs(boundAddress.sin_port);
        }
    } catch (...) {
        ::close(fd);
        throw;
    }

    listenFd_ = fd;
}

// [INTV:EDGE] while(true)로 EAGAIN을 만날 때까지 반복 accept하는 이유: 한 번의 이벤트 통지 사이에
// 여러 연결이 backlog 큐에 쌓였을 수 있어, 한 번에 다 받아들여야 다음 통지를 기다리며 방치되는
// 연결이 없다 (에지 트리거형 이벤트 모델에서 특히 중요한 "드레이닝" 패턴).
void Server::acceptReadyClients()
{
    while (true) {
        sockaddr_storage peerStorage;
        std::memset(&peerStorage, 0, sizeof(peerStorage));
        socklen_t peerLength = sizeof(peerStorage);
        int clientFd =
            ::accept(listenFd_, reinterpret_cast<sockaddr*>(&peerStorage), &peerLength);

        if (clientFd == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            reportError(std::string("accept: ") + std::strerror(errno));
            return;
        }

        // [INTV:EDGE] 연결 수 상한(maxConnections)을 넘으면 accept는 해놓고 바로 닫아버림 — 리소스
        // 고갈을 막는 방어적 설계.
        if (config_.maxConnections != 0 && connections_.size() >= config_.maxConnections) {
            rejectReadyClient();
            ::close(clientFd);
            continue;
        }

        try {
            setCloseOnExec(clientFd);
            setNonBlocking(clientFd);
            setNoSigPipe(clientFd);

            std::unique_ptr<Connection> connection(new Connection(
                clientFd,
                formatPeerAddress(peerStorage),
                config_.maxLineLength,
                config_.maxPendingBytes));
            // [INTV:TRAP] 소유권이 방금 만든 unique_ptr(connection)로 넘어갔으므로, 이 catch
            // 블록에서 clientFd를 또 close()하지 않도록 -1로 표시해둔다. 이 대입을 빼먹으면 정상
            // 경로에서도 catch에 들어갈 경우 이미 Connection이 소유한 fd를 이중으로 close()하게 된다.
            clientFd = -1;

            const int fd = connection->fd();
            const std::pair<std::unordered_map<int, std::unique_ptr<Connection> >::iterator, bool>
                inserted = connections_.emplace(fd, std::move(connection));
            if (!inserted.second) {
                throw std::logic_error("accepted descriptor is already registered");
            }
            try {
                eventManager_->addFd(fd, EventInterest::Read);
            } catch (...) {
                // [INTV:EDGE] 이벤트 등록이 실패하면 맵에 넣어둔 연결도 되돌려서(erase) "이벤트
                // 매니저는 모르는데 목록에는 남아있는" 불일치 상태를 방지한다.
                connections_.erase(inserted.first);
                throw;
            }
            ++metrics_.acceptedConnections;

            if (onConnect_) {
                try {
                    onConnect_(*inserted.first->second);
                } catch (const std::exception& exception) {
                    // [INTV:EDGE] 상위 콜백이 예외를 던져도 서버 루프 자체는 죽지 않게 잡아내고,
                    // 해당 연결만 종료 요청 상태로 표시한다 — 한 클라이언트의 오류가 서버 전체를
                    // 끌고 내려가지 않게 하는 격리(isolation) 원칙.
                    reportError(exception.what());
                    Connection* current = findConnection(fd);
                    if (current != NULL) {
                        current->requestClose("connect handler error");
                    }
                }
            }
            refreshInterest(fd);
        } catch (const std::exception& exception) {
            if (clientFd != -1) {
                ::close(clientFd);
            }
            reportError(exception.what());
        }
    }
}

void Server::rejectReadyClient()
{
    reportError("connection rejected: max connection count reached");
}

// [INTV:EDGE] 클라이언트 연결 하나에서 발생한 이벤트를 처리하는 핵심 디스패치 — 읽기 -> 콜백 호출 ->
// 쓰기 -> 연결종료 판단 순서로 진행한다. 중간중간 findConnection(fd)로 다시 조회하는 코드가 반복되는
// 이유: onLine_/onConnect_ 같은 상위 콜백이 disconnect()를 호출해 이 연결을 connections_에서 제거해
// 버릴 수 있어서, 들고 있던 connection 포인터가 그 사이 댕글링(dangling)될 수 있기 때문이다.
// - [TRAP] "콜백 호출 후 connection 포인터를 그대로 계속 쓴다"는 재구현은 use-after-free를 일으킨다.
//   콜백이 disconnect()를 부를 수 있는 지점마다 반드시 findConnection(fd)로 다시 확인하고, NULL이면
//   즉시 반환할 것.
void Server::handleClientEvent(const Event& event)
{
    std::unordered_map<int, std::unique_ptr<Connection> >::iterator found =
        connections_.find(event.fd);
    if (found == connections_.end()) {
        return;
    }

    const int fd = found->second->fd();
    Connection* connection = found->second.get();

    if (event.error) {
        disconnect(fd, eventErrorMessage(event));
        return;
    }

    if (hasInterest(event.interests, EventInterest::Read) && !connection->closeRequested()) {
        Connection::ReadResult readResult = connection->readAvailable();
        if (readResult.hasError) {
            disconnect(fd, readResult.error);
            return;
        }

        // 한 번의 읽기로 여러 줄이 도착했을 수 있어 한 줄씩 콜백에 넘긴다.
        for (std::vector<std::string>::const_iterator line = readResult.lines.begin();
             line != readResult.lines.end();
             ++line) {
            ++metrics_.linesReceived;
            if (onLine_) {
                try {
                    onLine_(*connection, *line);
                } catch (const std::exception& exception) {
                    reportError(exception.what());
                    Connection* current = findConnection(fd);
                    if (current != NULL) {
                        current->requestClose("line handler error");
                    }
                }
            }
            // 콜백 도중 연결이 사라졌을 수 있으니 다시 조회 — 사라졌다면 남은 줄 처리를 포기하고
            // 즉시 반환한다.
            connection = findConnection(fd);
            if (connection == NULL) {
                return;
            }
            if (connection->closeRequested()) {
                break;
            }
        }

        if (readResult.peerClosed) {
            connection->requestClose("peer closed connection");
        }
    }

    connection = findConnection(fd);
    if (connection == NULL) {
        return;
    }

    if (hasInterest(event.interests, EventInterest::Write) && connection->wantsWrite()) {
        Connection::WriteResult writeResult = connection->flushPending();
        if (writeResult.hasError) {
            disconnect(fd, writeResult.error);
            return;
        }
        connection = findConnection(fd);
        if (connection == NULL) {
            return;
        }
    }

    // [INTV:ARCH] 상대가 연결을 끊었어도(hangup) 아직 보낼 데이터가 버퍼에 남아있다면(wantsWrite)
    // 바로 끊지 않고 먼저 흘려보낼 기회를 준다 — 마지막 응답을 보내고 나서 정리하는 우아한 종료
    // (graceful shutdown) 설계.
    if (event.hangup && !connection->wantsWrite()) {
        disconnect(fd, "peer hangup");
        return;
    }

    refreshInterest(fd);
}

// [INTV:ARCH] 이 연결의 현재 상태(닫아야 하는지, 보낼 데이터가 남았는지)를 보고 epoll/kqueue에 등록된
// 관심사(Read/Write)를 다시 계산해 갱신한다 — 이벤트 루프와 연결 생명주기를 이어주는 핵심 로직.
// - [FLOW] 1. 종료 요청 + 송신 완료면 즉시 disconnect -> 2. 종료 요청 중이면 Read는 빼고 Write만
//   유지(새 입력은 안 받되 남은 출력은 다 보냄) -> 3. wantsWrite()면 Write 관심사 추가 -> 4. 최종
//   interests로 eventManager_->updateFd 호출
// - [TRAP] 종료 요청 상태에서도 Read 관심사를 계속 켜두면, 이미 처리하지 않을 연결의 입력을 계속
//   읽어들이는 낭비가 생긴다. "종료 중엔 쓰기만"이 이 함수의 핵심 불변조건이다.
bool Server::refreshInterest(int fd)
{
    Connection* connection = findConnection(fd);
    if (connection == NULL) {
        return false;
    }

    if (connection->closeRequested() && !connection->wantsWrite()) {
        disconnect(fd, connection->closeReason());
        return false;
    }

    EventInterest interests =
        connection->closeRequested() ? EventInterest::Write : EventInterest::Read;
    if (connection->wantsWrite()) {
        interests |= EventInterest::Write;
    }
    try {
        eventManager_->updateFd(fd, interests);
    } catch (const std::exception& exception) {
        reportError(exception.what());
        disconnect(fd, "event interest update failed");
        return false;
    }
    return true;
}

void Server::closeListenSocket() noexcept
{
    if (listenFd_ != -1) {
        if (eventManager_) {
            try {
                eventManager_->removeFd(listenFd_);
            } catch (...) {
            }
        }
        ::close(listenFd_);
        listenFd_ = -1;
    }
}

// [INTV:EDGE] fd 목록을 먼저 별도 벡터로 복사해두고 나서 disconnect를 호출하는 이유: disconnect()가
// connections_ 맵에서 항목을 erase하는데, 맵을 순회하는 도중 그 맵 자체를 수정하면 순회 중인 반복자가
// 무효화(iterator invalidation)되어 정의되지 않은 동작이 날 수 있다.
// - [TRAP] connections_를 직접 순회하며 그 안에서 disconnect(erase)를 호출하도록 재구현하면, 다음
//   ++it가 이미 무효화된 반복자를 증가시키는 미정의 동작이 된다. 반드시 키 목록을 스냅샷으로 뜬 뒤
//   그 스냅샷을 순회하며 원본 맵을 수정할 것.
void Server::closeAllConnections()
{
    std::vector<int> fds;
    fds.reserve(connections_.size());
    for (std::unordered_map<int, std::unique_ptr<Connection> >::const_iterator it =
             connections_.begin();
         it != connections_.end();
         ++it) {
        fds.push_back(it->first);
    }

    for (std::vector<int>::const_iterator it = fds.begin(); it != fds.end(); ++it) {
        disconnect(*it, "server stopped");
    }
}

void Server::reportError(const std::string& message) const noexcept
{
    if (onError_) {
        try {
            onError_(message);
        } catch (...) {
        }
    }
}

} // namespace irc
