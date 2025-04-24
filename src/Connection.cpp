#include "Connection.hpp"

#include <unistd.h>

#include <utility>

namespace irc {

Connection::Connection(int fd, std::string peerAddress, std::size_t maxLineLength)
    : fd_(fd)
    , peerAddress_(std::move(peerAddress))
    , writeOffset_(0)
    , maxLineLength_(maxLineLength == 0 ? 512 : maxLineLength)
    , peerClosed_(false)
    , closeRequested_(false)
{
}

Connection::~Connection()
{
    closeFd();
}

Connection::Connection(Connection&& other) noexcept
    : fd_(other.fd_)
    , peerAddress_(std::move(other.peerAddress_))
    , readBuffer_(std::move(other.readBuffer_))
    , writeBuffer_(std::move(other.writeBuffer_))
    , writeOffset_(other.writeOffset_)
    , maxLineLength_(other.maxLineLength_)
    , peerClosed_(other.peerClosed_)
    , closeRequested_(other.closeRequested_)
    , closeReason_(std::move(other.closeReason_))
{
    other.fd_ = -1;
    other.writeOffset_ = 0;
}

Connection& Connection::operator=(Connection&& other) noexcept
{
    if (this != &other) {
        closeFd();
        fd_ = other.fd_;
        peerAddress_ = std::move(other.peerAddress_);
        readBuffer_ = std::move(other.readBuffer_);
        writeBuffer_ = std::move(other.writeBuffer_);
        writeOffset_ = other.writeOffset_;
        maxLineLength_ = other.maxLineLength_;
        peerClosed_ = other.peerClosed_;
        closeRequested_ = other.closeRequested_;
        closeReason_ = std::move(other.closeReason_);

        other.fd_ = -1;
        other.writeOffset_ = 0;
    }
    return *this;
}

int Connection::fd() const noexcept
{
    return fd_;
}

const std::string& Connection::peerAddress() const noexcept
{
    return peerAddress_;
}

bool Connection::wantsWrite() const noexcept
{
    return writeOffset_ < writeBuffer_.size();
}

std::size_t Connection::pendingBytes() const noexcept
{
    return writeBuffer_.size() - writeOffset_;
}

void Connection::closeFd() noexcept
{
    if (fd_ != -1) {
        ::close(fd_);
        fd_ = -1;
    }
}

} // namespace irc
