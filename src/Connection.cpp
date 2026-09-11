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

// [INTV:TRADE_OFF] MSG_NOSIGNAL: 리눅스에서 send() 호출 단위로 "상대가 끊겼어도 SIGPIPE를 일으키지
// 말라"고 지시하는 플래그. Server.cpp의 SO_NOSIGPIPE(macOS/BSD, 소켓 단위 설정)와 목적은 같고 적용
// 방식만 다른 플랫폼별 이식성 이슈 — 둘 중 하나만 재구현하면 다른 플랫폼에서 죽은 연결에 쓰기를
// 시도할 때 프로세스 전체가 SIGPIPE로 죽을 수 있다.
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
    // [INTV:ARCH] std::function은 bool로 변환 가능(호출 가능한 대상을 담고 있으면 true) — 호출부가
    // 커스텀 sendOperation을 안 넘겼으면 진짜 send()를 감싼 기본 함수로 대체한다.
    , sendOperation_(sendOperation ? std::move(sendOperation) : SendOperation(sendBytes))
    , peerClosed_(false)
    , closeRequested_(false)
{
}

Connection::~Connection()
{
    closeFd();
}

// [INTV:EDGE] 이동 생성자: 리소스(fd, 버퍼)를 그대로 가져오고, other는 소멸자가 fd를 또 닫지 못하도록
// -1로 무효화해둔다.
// - [TRAP] other.fd_ = -1을 빼먹으면, 이동된 뒤의 원본 객체(other)가 소멸될 때 옮겨간 fd를 또 한 번
//   close()하는 이중 해제가 발생한다. "이동 후 원본은 유효하지만 비어있는 상태"를 반드시 만들 것.
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

// [INTV:EDGE] 이동 대입: 자기 자신의 fd를 먼저 닫은 뒤(this != &other일 때만) other의 리소스를 가져온다.
// - [TRAP] this != &other 체크 없이 자기 자신에게 이동 대입하면, closeFd()가 먼저 fd_를 닫아버려서
//   이후 "other"(사실은 this)의 fd_를 읽을 때 이미 닫힌(-1도 아닌 무효) fd를 쓰게 된다.
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

// [INTV:FLOW] TCP는 "바이트 스트림"이라 한 번의 recv가 메시지 경계와 무관하게 잘려서 온다 — 이번
// 이벤트 틱에서 커널 버퍼에 쌓인 데이터를 EAGAIN을 만날 때까지 전부 recv해 readBuffer_에 누적하고,
// 누적하는 족족 extractLines로 완성된 줄만 꺼낸다.
// - [FLOW] 1. recv 반복 -> 2. count>0이면 버퍼에 append 후 extractLines -> 3. count==0이면 상대가
//   정상 종료(FIN)한 것으로 처리 -> 4. EINTR이면 재시도, EAGAIN/EWOULDBLOCK이면 정상 종료(더 읽을 게
//   없음) -> 5. 그 외 에러는 연결 종료 요청
// - [TRAP] count==0을 에러로 취급하면(예: hasError=true) 정상적인 연결 종료를 에러 로그로 오분류하게
//   된다. recv()==0은 "상대가 정상적으로 닫았다"는 뜻이지 실패가 아니다.
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

// [INTV:FLOW] readAvailable과 대칭되는 구조: 쓰기 가능 이벤트를 받았을 때 호출되어, writeOffset_부터
// 이어서 write 버퍼를 최대한 흘려보내다 EAGAIN을 만나면 멈춘다.
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
            // [INTV:EDGE] 논블로킹 send는 요청한 만큼을 다 못 보내고 일부만(부분 전송) 보낼 수 있다 —
            // 보낸 만큼만 오프셋을 전진시키고, 남은 부분은 다음 쓰기 가능 이벤트에서 이어서 보낸다.
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
        // [INTV:PERF] writeOffset_ 앞부분(이미 보낸 바이트)이 문자열에 계속 남아있으면 버퍼가
        // 무한정 커진다. erase(0, n)은 나머지 전체를 앞으로 당기는 O(n) 연산이라 매번 하면 비효율적
        // 이므로, 일정 크기(16384)를 넘었을 때만 압축해 잦은 재할당/복사를 피한다.
        if (writeOffset_ > 16384) {
            writeBuffer_.erase(0, writeOffset_);
            writeOffset_ = 0;
        }
    }

    return result;
}

// [INTV:EDGE] 백프레셔(backpressure) 정책: 큐에 쌓인 바이트가 maxPendingBytes_를 넘어서면 받아주지
// 않고 연결 종료를 요청한다 — 상대가 못 받아가는 속도로 계속 밀어넣는(느린 수신자) 상황에서 서버
// 메모리가 무한정 늘어나는 것을 막는다.
bool Connection::queueRaw(const std::string& bytes)
{
    if (!canAppendPending(bytes.size())) {
        requestClose("outbound queue limit exceeded");
        return false;
    }
    writeBuffer_.append(bytes);
    return true;
}

// [INTV:EDGE] IRC 등 텍스트 기반 라인 프로토콜은 각 메시지를 CRLF("\r\n")로 끝맺는 게 관례 — 호출자가
// 이미 붙여 보냈을 수도 있는 개행을 일단 잘라내고 항상 정규화된 CRLF를 새로 붙인다.
// - [TRAP] "본문 길이 + 2"를 한 번에 더해서 한도 체크하면 ConnectionLimits.hpp에서 다룬 것과 같은
//   오버플로 함정에 걸릴 수 있다. 그래서 여기서도 본문과 CRLF 2바이트를 두 번에 나눠 각각
//   canAppendPending으로 검증한다.
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

// [INTV:EDGE] readBuffer_에 누적된 바이트에서 개행('\n') 기준으로 완성된 줄을 뽑아내는 파서. 개행이
// 아직 안 왔으면 버퍼에 그대로 두고 다음 recv를 기다리되, 개행 없이 버퍼가 maxLineLength_를 넘도록
// 계속 쌓이면 악의적이거나 오동작하는 클라이언트가 메모리를 무한정 소모시킬 수 있으므로 그 시점에
// 에러 처리하고 연결을 끊는다(입력 크기 제한 방어).
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
        // CRLF/LF 겸용: 개행 앞에 '\r'가 붙어있으면 잘라내 순수 텍스트만 남긴다.
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
