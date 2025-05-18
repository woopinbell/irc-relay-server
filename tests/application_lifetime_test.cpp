#include "EventManager.hpp"
#include "IrcApplication.hpp"
#include "Server.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
class CapturedStderr {
public:
    CapturedStderr() : previous_(std::cerr.rdbuf(stream_.rdbuf())) {}
    ~CapturedStderr() { std::cerr.rdbuf(previous_); }
    std::string str() const { return stream_.str(); }
private:
    std::ostringstream stream_;
    std::streambuf* previous_;
};

class FakeEventManager : public irc::EventManager {
public:
    void addFd(int fd, irc::EventInterest interests) override { interests_[fd] = interests; }
    void updateFd(int fd, irc::EventInterest interests) override {
        if (interests_.find(fd) == interests_.end()) {
            throw std::runtime_error("updated an unregistered descriptor");
        }
        interests_[fd] = interests;
    }
    void removeFd(int fd) override { interests_.erase(fd); }
    std::vector<irc::Event> wait(int) override {
        std::vector<irc::Event> ready;
        ready.swap(events_);
        return ready;
    }
    void queueReadable(int fd) {
        irc::Event event;
        event.fd = fd;
        event.interests = irc::EventInterest::Read;
        events_.push_back(event);
    }
    int clientFd(int listenFd) const {
        for (std::unordered_map<int, irc::EventInterest>::const_iterator it = interests_.begin();
             it != interests_.end(); ++it) {
            if (it->first != listenFd) {
                return it->first;
            }
        }
        return -1;
    }
private:
    std::unordered_map<int, irc::EventInterest> interests_;
    std::vector<irc::Event> events_;
};

class ClientSocket {
public:
    explicit ClientSocket(std::uint16_t port) : fd_(::socket(AF_INET, SOCK_STREAM, 0)) {
        if (fd_ == -1) {
            throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
        }
        sockaddr_in address;
        std::memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1) {
            throw std::runtime_error(std::string("connect: ") + std::strerror(errno));
        }
    }
    ~ClientSocket() { if (fd_ != -1) { ::close(fd_); } }
    void sendRegistration() {
        const std::string bytes = "NICK tiny\r\nUSER tiny 0 * :Tiny User\r\n";
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const ssize_t count = ::send(fd_, bytes.data() + offset, bytes.size() - offset, 0);
            if (count > 0) {
                offset += static_cast<std::size_t>(count);
            } else if (count == -1 && errno == EINTR) {
                continue;
            } else {
                throw std::runtime_error(std::string("send: ") + std::strerror(errno));
            }
        }
    }
private:
    int fd_;
};

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void waitReadable(int fd) {
    pollfd descriptor;
    descriptor.fd = fd;
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    const int result = ::poll(&descriptor, 1, 1000);
    if (result <= 0 || (descriptor.revents & POLLIN) == 0) {
        throw std::runtime_error("accepted socket did not become readable");
    }
}

void acceptUntil(irc::Server& server, FakeEventManager& events, std::size_t expectedCount) {
    for (int attempt = 0; attempt < 10; ++attempt) {
        waitReadable(server.listenFd());
        events.queueReadable(server.listenFd());
        server.pollOnce(0);
        if (server.connectionCount() >= expectedCount) {
            return;
        }
    }
    throw std::runtime_error("accepted connection was not registered");
}

void registrationQueueFailureTest() {
    CapturedStderr captured;
    std::unique_ptr<FakeEventManager> ownedEvents(new FakeEventManager());
    FakeEventManager* events = ownedEvents.get();
    irc::Server::Config config;
    config.bindAddress = "127.0.0.1";
    config.port = 0;
    config.eventTimeoutMs = 0;
    config.maxPendingBytes = 1;
    irc::Server server(config, std::move(ownedEvents));
    RuntimeConfig runtime;
    IrcApplication app(server, "", runtime);
    server.setConnectHandler([&app](Connection& connection) { app.onConnect(connection); });
    server.setLineHandler([&app](Connection& connection, const std::string& line) { app.onLine(connection, line); });
    server.setDisconnectHandler([&app](Connection& connection, const std::string& reason) { app.onDisconnect(connection, reason); });
    server.start();

    ClientSocket client(server.port());
    acceptUntil(server, *events, 1);
    const int clientFd = events->clientFd(server.listenFd());
    require(clientFd != -1, "accepted connection was not registered");
    client.sendRegistration();
    waitReadable(clientFd);
    events->queueReadable(clientFd);
    server.pollOnce(0);

    require(server.connectionCount() == 0, "queue rejection left an application connection behind");
    require(server.isRunning(), "application queue rejection stopped the server");
    require(captured.str().find("event=client_registered") == std::string::npos,
            "registration was recorded after the connection was removed");
    server.setConnectHandler(Server::ConnectHandler());
    server.setLineHandler(Server::LineHandler());
    server.setDisconnectHandler(Server::DisconnectHandler());
    server.stop();
}
} // namespace

int main() {
    try {
        registrationQueueFailureTest();
    } catch (const std::exception& exception) {
        std::cerr << "application lifetime test failed: " << exception.what() << '\n';
        return 1;
    }
    std::cout << "application lifetime test passed\n";
    return 0;
}
