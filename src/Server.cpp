#include "Server.hpp"

#include <utility>

namespace irc {

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

} // namespace irc
