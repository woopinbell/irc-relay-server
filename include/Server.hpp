#ifndef IRC_SERVER_HPP
#define IRC_SERVER_HPP

#include "Connection.hpp"
#include "EventManager.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace irc {

// [INTV:ARCH] TCP 서버의 리액터 루프(accept + 이벤트 대기 + 디스패치)를 감싸는 클래스. 실제 프로토콜
// (IRC 명령 파싱 등)은 전혀 모르고, "연결이 생겼다/한 줄이 도착했다/연결이 끊겼다"는 이벤트만 콜백으로
// 상위에 통지한다 — 전송 계층과 애플리케이션 로직을 분리하는 설계.
class Server {
public:
    struct Config {
        std::string bindAddress = "0.0.0.0";
        std::uint16_t port = 6667;
        int backlog = 128;
        // [INTV:TRADE_OFF] eventTimeoutMs: 이벤트 대기(wait)가 한 번에 블로킹할 최대 시간 — 너무
        // 짧으면 CPU를 불필요하게 소모하고, 너무 길면 stop() 요청 반영이 늦어지는 트레이드오프.
        int eventTimeoutMs = 1000;
        // [INTV:EDGE] maxLineLength/maxPendingBytes: 클라이언트 한 줄 길이와 송신 버퍼 총량의 상한 —
        // 악성/느린 클라이언트로 인한 메모리 무한 증가를 막는 방어적 설계 (Connection 참고).
        std::size_t maxLineLength = 512;
        std::size_t maxPendingBytes = 1048576;
        std::size_t maxConnections = 256;
    };

    struct Metrics {
        std::size_t acceptedConnections = 0;
        std::size_t closedConnections = 0;
        std::size_t linesReceived = 0;
        std::size_t outboundQueueDrops = 0;
    };

    // [INTV:ARCH] std::function 타입 지우기(type erasure) 콜백 — 상위 계층(IRC 애플리케이션)이 이
    // 핸들러들을 등록해두면, Server는 프로토콜을 몰라도 이벤트 발생 시점에 그대로 호출해줄 수 있다.
    using ConnectHandler = std::function<void(Connection&)>;
    using LineHandler = std::function<void(Connection&, const std::string&)>;
    using DisconnectHandler = std::function<void(Connection&, const std::string&)>;
    using ErrorHandler = std::function<void(const std::string&)>;

    explicit Server(Config config);
    // [INTV:ARCH] 테스트나 특수 목적으로 EventManager 구현체를 직접 주입할 수 있는 생성자 — 플랫폼
    // 기본값(createDefault) 대신 가짜/커스텀 구현으로 교체 가능한 DI 지점.
    Server(Config config, std::unique_ptr<EventManager> eventManager);
    ~Server();

    // [INTV:EDGE] 복사 금지: 소켓 fd와 연결 목록을 실질적으로 소유하는 객체라, 복사를 허용하면 같은
    // fd를 두 인스턴스가 각자 관리하려 드는 이중 소유 문제가 생긴다.
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void start();
    // [INTV:ARCH] run(): start() 이후 stop()이 호출되기 전까지 pollOnce를 반복 호출하는 블로킹 루프.
    // pollOnce를 직접 반복 호출해도 되지만(임베딩 시 유용), run()은 그 흔한 패턴을 감싼 편의 API다.
    void run();
    void pollOnce(int timeoutMs = -1);
    void stop() noexcept;

    bool isRunning() const noexcept;
    const Config& config() const noexcept;
    int listenFd() const noexcept;
    std::uint16_t port() const noexcept;
    std::size_t connectionCount() const noexcept;
    const Metrics& metrics() const noexcept;

    void setConnectHandler(ConnectHandler handler);
    void setLineHandler(LineHandler handler);
    void setDisconnectHandler(DisconnectHandler handler);
    void setErrorHandler(ErrorHandler handler);

    bool sendTo(int fd, const std::string& line);
    bool queueRawTo(int fd, const std::string& bytes);
    void disconnect(int fd, const std::string& reason = "server disconnect");

    Connection* findConnection(int fd);
    const Connection* findConnection(int fd) const;

private:
    Config config_;
    int listenFd_;
    std::unique_ptr<EventManager> eventManager_;
    // [INTV:ARCH] fd를 키로 Connection을 소유하는 맵 — unique_ptr을 값으로 둬서 이 맵이 각 Connection의
    // 유일한 소유자가 되고, erase되면 자동으로 소멸(RAII)된다.
    std::unordered_map<int, std::unique_ptr<Connection> > connections_;
    bool running_;
    bool stopRequested_;
    Metrics metrics_;

    ConnectHandler onConnect_;
    LineHandler onLine_;
    DisconnectHandler onDisconnect_;
    ErrorHandler onError_;

    void createListenSocket();
    void acceptReadyClients();
    void rejectReadyClient();
    void handleClientEvent(const Event& event);
    bool refreshInterest(int fd);
    void closeListenSocket() noexcept;
    void closeAllConnections();
    void reportError(const std::string& message) const noexcept;
};

} // namespace irc

using Server = irc::Server;

#endif // IRC_SERVER_HPP
