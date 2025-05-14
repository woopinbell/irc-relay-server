#ifndef IRC_CONNECTION_HPP
#define IRC_CONNECTION_HPP

#include <cstddef>
#include <string>
#include <vector>

namespace irc {

class Connection {
public:
    struct ReadResult {
        std::vector<std::string> lines;
        bool wouldBlock = false;
        bool peerClosed = false;
        bool hasError = false;
        std::string error;
    };

    struct WriteResult {
        bool finished = true;
        bool wouldBlock = false;
        bool hasError = false;
        std::string error;
    };

    Connection(int fd,
               std::string peerAddress,
               std::size_t maxLineLength = 512,
               std::size_t maxPendingBytes = 1048576);
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;

    int fd() const noexcept;
    const std::string& peerAddress() const noexcept;
    bool wantsWrite() const noexcept;
    std::size_t pendingBytes() const noexcept;

    ReadResult readAvailable();
    WriteResult flushPending();

    bool queueRaw(const std::string& bytes);
    bool queueLine(const std::string& line);

    void requestClose(std::string reason = "connection close requested");
    bool closeRequested() const noexcept;
    const std::string& closeReason() const noexcept;
    bool peerClosed() const noexcept;

private:
    int fd_;
    std::string peerAddress_;
    std::string readBuffer_;
    std::string writeBuffer_;
    std::size_t writeOffset_;
    std::size_t maxLineLength_;
    std::size_t maxPendingBytes_;
    bool peerClosed_;
    bool closeRequested_;
    std::string closeReason_;

    void closeFd() noexcept;
    bool extractLines(ReadResult& result);
    bool canAppendPending(std::size_t byteCount) const noexcept;
};

} // namespace irc

using Connection = irc::Connection;

#endif // IRC_CONNECTION_HPP
