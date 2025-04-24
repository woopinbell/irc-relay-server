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

bool Connection::extractLines(ReadResult& result)
{
    while (true) {
        const std::string::size_type newline = readBuffer_.find('\n');
        if (newline == std::string::npos) {
            if (readBuffer_.size() > maxLineLength_) {
                result.hasError = true;
                result.error = "incoming line exceeds maximum length";
                requestClose(result.error);
                readBuffer_.clear();
                return false;
            }
            return true;
        }

        if (newline + 1 > maxLineLength_) {
            result.hasError = true;
            result.error = "incoming line exceeds maximum length";
            requestClose(result.error);
            readBuffer_.clear();
            return false;
        }

        std::string line = readBuffer_.substr(0, newline);
        readBuffer_.erase(0, newline + 1);
        if (!line.empty() && line[line.size() - 1] == '\r') {
            line.erase(line.size() - 1);
        }
        result.lines.push_back(line);
    }
}

} // namespace irc
