#include "EventManager.hpp"
#include "Server.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

class FakeEventManager : public irc::EventManager {
public:
    void addFd(int fd, irc::EventInterest interests) override
    {
        ++addCalls_;
        if (failNextAdd_) {
            failNextAdd_ = false;
            throw std::runtime_error("injected add failure");
        }
        interests_[fd] = interests;
    }

    void updateFd(int fd, irc::EventInterest interests) override
    {
        if (failNextUpdate_) {
            failNextUpdate_ = false;
            throw std::runtime_error("injected update failure");
        }
        if (interests_.find(fd) == interests_.end()) {
            throw std::runtime_error("updated an unregistered descriptor");
        }
        interests_[fd] = interests;
    }

    void removeFd(int fd) override
    {
        interests_.erase(fd);
    }

    std::vector<irc::Event> wait(int) override
    {
        std::vector<irc::Event> result;
        result.swap(events_);
        return result;
    }

    void failNextAdd()
    {
        failNextAdd_ = true;
    }

    void failNextUpdate()
    {
        failNextUpdate_ = true;
    }

    std::size_t addCallCount() const
    {
        return addCalls_;
    }

    void queueReadable(int fd)
    {
        irc::Event event;
        event.fd = fd;
        event.interests = irc::EventInterest::Read;
        events_.push_back(event);
    }

    bool contains(int fd) const
    {
        return interests_.find(fd) != interests_.end();
    }

    int clientFd(int listenFd) const
    {
        for (std::unordered_map<int, irc::EventInterest>::const_iterator it = interests_.begin();
             it != interests_.end();
             ++it) {
            if (it->first != listenFd) {
                return it->first;
            }
        }
        return -1;
    }

private:
    std::unordered_map<int, irc::EventInterest> interests_;
    std::vector<irc::Event> events_;
    std::size_t addCalls_ = 0;
    bool failNextAdd_ = false;
    bool failNextUpdate_ = false;
};

class ClientSocket {
public:
    explicit ClientSocket(std::uint16_t port)
        : fd_(::socket(AF_INET, SOCK_STREAM, 0))
    {
        if (fd_ == -1) {
            throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
        }

        sockaddr_in address;
        std::memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1) {
            const std::string message = std::string("connect: ") + std::strerror(errno);
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error(message);
        }
    }

    ~ClientSocket()
    {
        if (fd_ != -1) {
            ::close(fd_);
        }
    }

    void sendLine(const std::string& line)
    {
        const std::string bytes = line + "\r\n";
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const ssize_t count = ::send(fd_, bytes.data() + offset, bytes.size() - offset, 0);
            if (count > 0) {
                offset += static_cast<std::size_t>(count);
                continue;
            }
            if (count == -1 && errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("send: ") + std::strerror(errno));
        }
    }

private:
    int fd_;
};

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

irc::Server::Config loopbackConfig()
{
    irc::Server::Config config;
    config.bindAddress = "127.0.0.1";
    config.port = 0;
    config.eventTimeoutMs = 0;
    return config;
}

void acceptOne(irc::Server& server, FakeEventManager& events)
{
    const std::size_t initialAddCalls = events.addCallCount();
    for (int attempt = 0; attempt < 100; ++attempt) {
        events.queueReadable(server.listenFd());
        server.pollOnce(0);
        if (events.addCallCount() > initialAddCalls) {
            return;
        }
        ::usleep(1000);
    }
    throw std::runtime_error("client connection was not accepted");
}

void waitReadable(int fd)
{
    pollfd descriptor;
    descriptor.fd = fd;
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    while (true) {
        const int count = ::poll(&descriptor, 1, 1000);
        if (count > 0 && (descriptor.revents & POLLIN) != 0) {
            return;
        }
        if (count == -1 && errno == EINTR) {
            continue;
        }
        throw std::runtime_error("accepted socket did not become readable");
    }
}

void registrationRollbackTest()
{
    std::unique_ptr<FakeEventManager> ownedEvents(new FakeEventManager());
    FakeEventManager* events = ownedEvents.get();
    irc::Server server(loopbackConfig(), std::move(ownedEvents));
    server.start();

    events->failNextAdd();
    ClientSocket client(server.port());
    acceptOne(server, *events);

    require(server.connectionCount() == 0, "failed registration left a connection behind");
    require(events->clientFd(server.listenFd()) == -1,
            "failed registration left an event descriptor behind");
}

void connectCallbackLifetimeTest()
{
    std::unique_ptr<FakeEventManager> ownedEvents(new FakeEventManager());
    FakeEventManager* events = ownedEvents.get();
    irc::Server server(loopbackConfig(), std::move(ownedEvents));
    server.setErrorHandler([](const std::string&) { throw std::runtime_error("error handler"); });
    server.setConnectHandler([&server](irc::Connection& connection) {
        server.disconnect(connection.fd(), "callback disconnect");
        throw std::runtime_error("connect callback");
    });
    server.start();

    ClientSocket client(server.port());
    acceptOne(server, *events);

    require(server.connectionCount() == 0,
            "connect callback accessed a connection after removing it");
}

void lineCallbackLifetimeTest()
{
    std::unique_ptr<FakeEventManager> ownedEvents(new FakeEventManager());
    FakeEventManager* events = ownedEvents.get();
    irc::Server server(loopbackConfig(), std::move(ownedEvents));
    server.setLineHandler([&server](irc::Connection& connection, const std::string&) {
        server.disconnect(connection.fd(), "callback disconnect");
        throw std::runtime_error("line callback");
    });
    server.start();

    ClientSocket client(server.port());
    acceptOne(server, *events);
    const int clientFd = events->clientFd(server.listenFd());
    require(clientFd != -1, "accepted connection was not registered");

    client.sendLine("PING :lifetime");
    waitReadable(clientFd);
    events->queueReadable(clientFd);
    server.pollOnce(0);

    require(server.connectionCount() == 0,
            "line callback accessed a connection after removing it");
}

void interestUpdateRollbackTest()
{
    std::unique_ptr<FakeEventManager> ownedEvents(new FakeEventManager());
    FakeEventManager* events = ownedEvents.get();
    irc::Server server(loopbackConfig(), std::move(ownedEvents));
    server.start();

    ClientSocket client(server.port());
    acceptOne(server, *events);
    const int clientFd = events->clientFd(server.listenFd());
    require(clientFd != -1, "accepted connection was not registered");

    events->failNextUpdate();
    require(!server.sendTo(clientFd, "NOTICE * :queued"),
            "send succeeded after its write interest update failed");
    require(server.connectionCount() == 0, "update failure left a connection behind");
    require(!events->contains(clientFd), "update failure left an event descriptor behind");
}

void queueLimitCloseTest()
{
    std::unique_ptr<FakeEventManager> ownedEvents(new FakeEventManager());
    FakeEventManager* events = ownedEvents.get();
    irc::Server::Config config = loopbackConfig();
    config.maxPendingBytes = 4;
    irc::Server server(config, std::move(ownedEvents));
    server.start();

    ClientSocket client(server.port());
    acceptOne(server, *events);
    const int clientFd = events->clientFd(server.listenFd());
    require(clientFd != -1, "accepted connection was not registered");

    require(!server.sendTo(clientFd, "payload"), "oversized output was accepted");
    require(server.connectionCount() == 0,
            "queue rejection did not finish the requested connection close");
    require(!events->contains(clientFd), "queue rejection left an event descriptor behind");
}

} // namespace

int main()
{
    try {
        registrationRollbackTest();
        connectCallbackLifetimeTest();
        lineCallbackLifetimeTest();
        interestUpdateRollbackTest();
        queueLimitCloseTest();
    } catch (const std::exception& exception) {
        std::cerr << "server lifetime test failed: " << exception.what() << '\n';
        return 1;
    }

    std::cout << "server lifetime test passed\n";
    return 0;
}
