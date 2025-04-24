#include "Connection.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace irc {
namespace {

std::string errorMessage(const char* operation)
{
    std::string message(operation);
    message += ": ";
    message += std::strerror(errno);
    return message;
}

} // namespace

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

Connection::ReadResult Connection::readAvailable()
{
    ReadResult result;
    char buffer[4096];

    while (true) {
        const ssize_t count = ::recv(fd_, buffer, sizeof(buffer), 0);
        if (count > 0) {
            readBuffer_.append(buffer, static_cast<std::size_t>(count));
            if (!extractLines(result)) {
                break;
            }
            continue;
        }
        if (count == 0) {
            peerClosed_ = true;
            result.peerClosed = true;
            requestClose("peer closed connection");
            break;
        }

        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            result.wouldBlock = true;
            break;
        }

        result.hasError = true;
        result.error = errorMessage("recv");
        requestClose(result.error);
        break;
    }

    return result;
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
