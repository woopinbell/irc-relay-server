#include "Connection.hpp"
#include "../src/ConnectionLimits.hpp"

#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

struct SendStep {
    enum Kind {
        Bytes,
        Error,
        Zero
    };

    Kind kind;
    std::size_t byteCount;
    int errorNumber;

    static SendStep bytes(std::size_t byteCount)
    {
        return SendStep{Bytes, byteCount, 0};
    }

    static SendStep error(int errorNumber)
    {
        return SendStep{Error, 0, errorNumber};
    }

    static SendStep zero()
    {
        return SendStep{Zero, 0, 0};
    }
};

class ScriptedSender {
public:
    explicit ScriptedSender(std::vector<SendStep> steps)
        : steps_(std::move(steps))
        , index_(0)
    {
    }

    ssize_t send(int, const void* data, std::size_t size, int)
    {
        if (index_ >= steps_.size()) {
            std::cerr << "unexpected send call" << std::endl;
            std::abort();
        }

        const SendStep step = steps_[index_++];
        if (step.kind == SendStep::Error) {
            errno = step.errorNumber;
            return -1;
        }
        if (step.kind == SendStep::Zero) {
            return 0;
        }
        if (step.byteCount > size) {
            return static_cast<ssize_t>(step.byteCount);
        }

        bytes_.append(static_cast<const char*>(data), step.byteCount);
        return static_cast<ssize_t>(step.byteCount);
    }

    const std::string& bytes() const
    {
        return bytes_;
    }

    std::size_t calls() const
    {
        return index_;
    }

private:
    std::vector<SendStep> steps_;
    std::size_t index_;
    std::string bytes_;
};

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(1);
    }
}

Connection::SendOperation operationFor(ScriptedSender& sender)
{
    return [&sender](int fd, const void* data, std::size_t size, int flags) {
        return sender.send(fd, data, size, flags);
    };
}

void testLimitArithmetic()
{
    const std::size_t maximum = std::numeric_limits<std::size_t>::max();

    require(irc::detail::canAppendPending(0, maximum, maximum),
            "an empty queue must accept its exact maximum");
    require(!irc::detail::canAppendPending(1, maximum, maximum),
            "limit checks must not wrap when the appended size overflows addition");
    require(!irc::detail::canAppendPending(maximum, 1, maximum),
            "a full queue must reject one additional byte");
    require(!irc::detail::canAppendPending(maximum, 0, maximum - 1),
            "an already-invalid queue size must be rejected");
}

void testHardLimitAndLineTerminator()
{
    ScriptedSender sender({SendStep::bytes(5)});
    Connection connection(-1, "test", 512, 5, operationFor(sender));

    require(connection.queueLine("abc\n"), "line plus CRLF must fit the exact limit");
    require(connection.pendingBytes() == 5, "line terminators must count toward the limit");
    require(!connection.queueRaw("x"), "one byte beyond the hard limit must be rejected");
    require(connection.closeRequested(), "limit overflow must request connection closure");
    require(connection.pendingBytes() == 5, "rejected bytes must not mutate the queue");

    const Connection::WriteResult result = connection.flushPending();
    require(result.finished && !result.hasError, "an exact-limit queue must still flush");
    require(sender.bytes() == "abc\r\n", "queueLine must normalize its terminator");
}

void testInterruptedAndPartialSendState()
{
    ScriptedSender sender({
        SendStep::error(EINTR),
        SendStep::bytes(2),
        SendStep::error(EAGAIN),
        SendStep::bytes(3),
        SendStep::error(EWOULDBLOCK),
        SendStep::bytes(7),
    });
    Connection connection(-1, "test", 512, 10, operationFor(sender));

    require(connection.queueRaw("abcdef"), "initial payload must be queued");

    Connection::WriteResult result = connection.flushPending();
    require(!result.finished && result.wouldBlock && !result.hasError,
            "EINTR must retry and EAGAIN must preserve pending output");
    require(connection.pendingBytes() == 4, "only successfully sent bytes may be consumed");
    require(sender.bytes() == "ab", "the first partial send must not duplicate data");

    require(connection.queueRaw("ghijkl"),
            "freed capacity after a partial send must be reusable up to the exact limit");
    require(connection.pendingBytes() == 10, "pending bytes must exclude the sent prefix");
    require(!connection.queueRaw("m"), "the refilled hard limit must reject another byte");
    require(connection.pendingBytes() == 10, "rejection must preserve the refilled queue");

    result = connection.flushPending();
    require(!result.finished && result.wouldBlock && !result.hasError,
            "a later partial send must also retain its offset");
    require(connection.pendingBytes() == 7, "the second partial send must advance once");

    result = connection.flushPending();
    require(result.finished && !result.wouldBlock && !result.hasError,
            "the remaining output must finish on the next writable event");
    require(connection.pendingBytes() == 0 && !connection.wantsWrite(),
            "a completed queue must reset its storage state");
    require(sender.bytes() == "abcdefghijkl",
            "retries must deliver each byte exactly once and in order");
    require(sender.calls() == 6, "the scripted syscall sequence must be exhausted");
}

void testZeroAndTerminalErrorPreservePendingBytes()
{
    ScriptedSender zeroSender({SendStep::zero(), SendStep::bytes(3)});
    Connection zeroConnection(-1, "test", 512, 8, operationFor(zeroSender));
    require(zeroConnection.queueRaw("abc"), "zero-send payload must be queued");

    Connection::WriteResult result = zeroConnection.flushPending();
    require(!result.finished && result.wouldBlock && !result.hasError,
            "a zero-byte send must retain the queue for a later writable event");
    require(zeroConnection.pendingBytes() == 3, "zero-byte send must consume nothing");

    result = zeroConnection.flushPending();
    require(result.finished && zeroSender.bytes() == "abc",
            "data retained after a zero-byte send must be delivered later");

    ScriptedSender errorSender({SendStep::error(EPIPE)});
    Connection errorConnection(-1, "test", 512, 8, operationFor(errorSender));
    require(errorConnection.queueRaw("xyz"), "terminal-error payload must be queued");

    result = errorConnection.flushPending();
    require(result.hasError && !result.finished, "a terminal send error must be reported");
    require(errorConnection.closeRequested(), "a terminal send error must request closure");
    require(errorConnection.pendingBytes() == 3, "a failed send must not consume queued bytes");
}

void testInvalidSendCountCannotAdvanceOffset()
{
    ScriptedSender sender({SendStep::bytes(4)});
    Connection connection(-1, "test", 512, 8, operationFor(sender));
    require(connection.queueRaw("abc"), "invalid-count payload must be queued");

    const Connection::WriteResult result = connection.flushPending();
    require(result.hasError && !result.finished,
            "a byte count larger than the request must be rejected");
    require(connection.closeRequested(), "an invalid byte count must request closure");
    require(connection.pendingBytes() == 3, "an invalid byte count must not corrupt the offset");
}

} // namespace

int main()
{
    testLimitArithmetic();
    testHardLimitAndLineTerminator();
    testInterruptedAndPartialSendState();
    testZeroAndTerminalErrorPreservePendingBytes();
    testInvalidSendCountCannotAdvanceOffset();

    std::cout << "Connection tests passed" << std::endl;
    return 0;
}
