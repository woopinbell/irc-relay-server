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

class Server {
public:
    struct Config {
        std::string bindAddress = "0.0.0.0";
        std::uint16_t port = 6667;
        int backlog = 128;
        int eventTimeoutMs = 1000;
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

    using ConnectHandler = std::function<void(Connection&)>;
    using LineHandler = std::function<void(Connection&, const std::string&)>;
    using DisconnectHandler = std::function<void(Connection&, const std::string&)>;
    using ErrorHandler = std::function<void(const std::string&)>;

    explicit Server(Config config);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void start();
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
    void refreshInterest(Connection& connection);
    void closeListenSocket() noexcept;
    void closeAllConnections();
    void reportError(const std::string& message) const;
};

} // namespace irc

using Server = irc::Server;

#endif // IRC_SERVER_HPP
