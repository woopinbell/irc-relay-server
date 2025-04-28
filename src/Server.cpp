#include "Server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <system_error>
#include <utility>

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

} // namespace irc
