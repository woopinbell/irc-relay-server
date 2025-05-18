#include "Connection.hpp"
#include "ConnectionLimits.hpp"

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

int sendFlags()
{
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

ssize_t sendBytes(int fd, const void* data, std::size_t size, int flags)
{
    return ::send(fd, data, size, flags);
}

} // namespace

Connection::Connection(int fd,
                       std::string peerAddress,
                       std::size_t maxLineLength,
                       std::size_t maxPendingBytes,
                       SendOperation sendOperation)
    : fd_(fd)
    , peerAddress_(std::move(peerAddress))
    , writeOffset_(0)
    , maxLineLength_(maxLineLength == 0 ? 512 : maxLineLength)
    , maxPendingBytes_(maxPendingBytes == 0 ? 1048576 : maxPendingBytes)
    , sendOperation_(sendOperation ? std::move(sendOperation) : SendOperation(sendBytes))
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
    , maxPendingBytes_(other.maxPendingBytes_)
    , sendOperation_(std::move(other.sendOperation_))
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
        maxPendingBytes_ = other.maxPendingBytes_;
        sendOperation_ = std::move(other.sendOperation_);
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

Connection::WriteResult Connection::flushPending()
{
    WriteResult result;

    while (wantsWrite()) {
        const char* data = writeBuffer_.data() + writeOffset_;
        const std::size_t size = writeBuffer_.size() - writeOffset_;
        const ssize_t count = sendOperation_
            ? sendOperation_(fd_, data, size, sendFlags())
            : sendBytes(fd_, data, size, sendFlags());

        if (count > 0) {
            const std::size_t sent = static_cast<std::size_t>(count);
            if (sent > size) {
                result.hasError = true;
                result.error = "send returned more bytes than requested";
                requestClose(result.error);
                break;
            }
            writeOffset_ += sent;
            continue;
        }
        if (count == 0) {
            result.wouldBlock = true;
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
        result.error = errorMessage("send");
        requestClose(result.error);
        break;
    }

    if (!wantsWrite()) {
        writeBuffer_.clear();
        writeOffset_ = 0;
        result.finished = true;
    } else {
        result.finished = false;
        if (writeOffset_ > 16384) {
            writeBuffer_.erase(0, writeOffset_);
            writeOffset_ = 0;
        }
    }

    return result;
}

bool Connection::queueRaw(const std::string& bytes)
{
    if (!canAppendPending(bytes.size())) {
        requestClose("outbound queue limit exceeded");
        return false;
    }
    writeBuffer_.append(bytes);
    return true;
}

bool Connection::queueLine(const std::string& line)
{
    std::size_t end = line.size();
    while (end > 0 && (line[end - 1] == '\r' || line[end - 1] == '\n')) {
        --end;
    }
    const std::size_t pending = pendingBytes();
    if (!detail::canAppendPending(pending, end, maxPendingBytes_)
        || !detail::canAppendPending(pending + end, 2, maxPendingBytes_)) {
        requestClose("outbound queue limit exceeded");
        return false;
    }
    writeBuffer_.append(line, 0, end);
    writeBuffer_.append("\r\n");
    return true;
}

void Connection::requestClose(std::string reason)
{
    closeRequested_ = true;
    closeReason_ = std::move(reason);
}

bool Connection::closeRequested() const noexcept
{
    return closeRequested_;
}

const std::string& Connection::closeReason() const noexcept
{
    return closeReason_;
}

bool Connection::peerClosed() const noexcept
{
    return peerClosed_;
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

bool Connection::canAppendPending(std::size_t byteCount) const noexcept
{
    return detail::canAppendPending(pendingBytes(), byteCount, maxPendingBytes_);
}

} // namespace irc
