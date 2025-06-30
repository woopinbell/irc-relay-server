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

    eventManager_ = EventManager::createDefault();
    createListenSocket();
    eventManager_->addFd(listenFd_, EventInterest::Read);
    stopRequested_ = false;
    running_ = true;
}

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

void Server::pollOnce(int timeoutMs)
{
    if (!running_) {
        throw std::logic_error("Server::pollOnce called before start");
    }

    const int effectiveTimeout = timeoutMs < 0 ? config_.eventTimeoutMs : timeoutMs;
    const std::vector<Event> events = eventManager_->wait(effectiveTimeout);

    for (std::vector<Event>::const_iterator it = events.begin(); it != events.end(); ++it) {
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

bool Server::sendTo(int fd, const std::string& line)
{
    Connection* connection = findConnection(fd);
    if (connection == NULL) {
        return false;
    }
    const bool queued = connection->queueLine(line);
    refreshInterest(*connection);
    return queued;
}

bool Server::queueRawTo(int fd, const std::string& bytes)
{
    Connection* connection = findConnection(fd);
    if (connection == NULL) {
        return false;
    }
    const bool queued = connection->queueRaw(bytes);
    refreshInterest(*connection);
    return queued;
}

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

void Server::createListenSocket()
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        throw std::system_error(errno, std::generic_category(), "socket");
    }

    try {
        setCloseOnExec(fd);
        setNonBlocking(fd);

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
            clientFd = -1;

            const int fd = connection->fd();
            eventManager_->addFd(fd, EventInterest::Read);
            Connection* connectionPtr = connection.get();
            connections_[fd] = std::move(connection);

            if (onConnect_) {
                try {
                    onConnect_(*connectionPtr);
                } catch (const std::exception& exception) {
                    reportError(exception.what());
                    connectionPtr->requestClose("connect handler error");
                }
            }
            if (connections_.find(fd) != connections_.end()) {
                refreshInterest(*connectionPtr);
            }
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

void Server::handleClientEvent(const Event& event)
{
    std::unordered_map<int, std::unique_ptr<Connection> >::iterator found =
        connections_.find(event.fd);
    if (found == connections_.end()) {
        return;
    }

    Connection* connection = found->second.get();
    const int fd = connection->fd();

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

        for (std::vector<std::string>::const_iterator line = readResult.lines.begin();
             line != readResult.lines.end();
             ++line) {
            if (onLine_) {
                try {
                    onLine_(*connection, *line);
                } catch (const std::exception& exception) {
                    reportError(exception.what());
                    connection->requestClose("line handler error");
                }
            }
            if (connections_.find(fd) == connections_.end()) {
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

    if (connections_.find(fd) == connections_.end()) {
        return;
    }

    if (hasInterest(event.interests, EventInterest::Write) && connection->wantsWrite()) {
        Connection::WriteResult writeResult = connection->flushPending();
        if (writeResult.hasError) {
            disconnect(fd, writeResult.error);
            return;
        }
    }

    if (event.hangup && !connection->wantsWrite()) {
        disconnect(fd, "peer hangup");
        return;
    }

    refreshInterest(*connection);
}

void Server::refreshInterest(Connection& connection)
{
    const int fd = connection.fd();

    if (connection.closeRequested() && !connection.wantsWrite()) {
        disconnect(fd, connection.closeReason());
        return;
    }

    EventInterest interests = connection.closeRequested() ? EventInterest::Write : EventInterest::Read;
    if (connection.wantsWrite()) {
        interests |= EventInterest::Write;
    }
    eventManager_->updateFd(fd, interests);
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

void Server::reportError(const std::string& message) const
{
    if (onError_) {
        onError_(message);
    }
}

} // namespace irc
