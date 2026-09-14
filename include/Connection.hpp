#ifndef IRC_CONNECTION_HPP
#define IRC_CONNECTION_HPP

#include <sys/types.h>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace irc {

// [INTV:ARCH] 논블로킹 TCP 소켓 하나를 감싸, 줄 단위 프로토콜(IRC처럼 개행으로 메시지를 구분)에 맞게
// 읽기 버퍼링(부분 수신 -> 완성된 줄 추출)과 쓰기 버퍼링(큐잉 -> 논블로킹 전송)을 대신 처리한다.
// Server는 이 클래스를 통해서만 소켓 I/O를 다룬다 — 원시 fd 조작을 이 한 클래스로 캡슐화.
class Connection {
public:
    // [INTV:ARCH] 실제 send() 시스템 콜을 함수 객체로 추상화한 타입 — 테스트에서 진짜 소켓 대신 가짜
    // send 동작(부분 전송, 에러 등)을 주입할 수 있게 하는 의존성 주입(DI) 지점.
    using SendOperation = std::function<ssize_t(int, const void*, std::size_t, int)>;

    struct ReadResult {
        std::vector<std::string> lines;
        bool wouldBlock = false;
        bool peerClosed = false;
        bool hasError = false;
        std::string error;
    };

    // [INTV:EDGE] finished가 false면 큐에 데이터가 남아있다는 뜻 — 논블로킹 소켓은 한 번의 flush로
    // 전부 못 보낼 수 있어, 다음 쓰기 가능 이벤트를 기다려 이어서 보내야 한다.
    struct WriteResult {
        bool finished = true;
        bool wouldBlock = false;
        bool hasError = false;
        std::string error;
    };

    Connection(int fd,
               std::string peerAddress,
               std::size_t maxLineLength = 512,
               std::size_t maxPendingBytes = 1048576,
               SendOperation sendOperation = SendOperation());
    ~Connection();

    // [INTV:ARCH] fd(OS 리소스)의 유일 소유권을 표현 — 복사를 허용하면 같은 fd를 두 객체가 각자
    // close()하려 들어 이중 해제가 되므로 복사는 삭제하고, 소유권을 넘기는 이동만 허용한다.
    // - [TRAP] std::unordered_map<int, unique_ptr<Connection>>이 아니라 값으로(Connection 직접) 담는
    //   컨테이너를 쓰려 하면, 컨테이너 재배치 시 이동 생성자가 필요해진다는 걸 놓치기 쉽다 — 이동
    //   시맨틱을 갖춘 이유가 바로 이런 컨테이너 저장 요구사항 때문이다.
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

    // [INTV:ARCH] [INTV:EDGE] 지연 종료(deferred close) 패턴: 호출 즉시 fd를 닫지 않고 "닫아야 함"
    // 표시만 해둔다 — 아직 보내지 못한 데이터가 write 버퍼에 남아있을 수 있어, 실제 정리는 Server
    // 쪽에서 남은 데이터를 다 흘려보낸 뒤(wantsWrite()가 false가 된 뒤) 수행한다.
    // - [TRAP] requestClose()에서 곧바로 close(fd_)까지 해버리면, 방금 write 버퍼에 쌓아둔 종료
    //   메시지(예: ERROR 응답)를 상대가 받기 전에 연결이 끊겨 클라이언트가 이유를 알 수 없게 된다.
    void requestClose(std::string reason = "connection close requested");
    bool closeRequested() const noexcept;
    const std::string& closeReason() const noexcept;
    bool peerClosed() const noexcept;

private:
    int fd_;
    std::string peerAddress_;
    std::string readBuffer_;
    std::string writeBuffer_;
    // [INTV:PERF] writeBuffer_ 중 이미 전송에 성공한 바이트 수 — 논블로킹 send는 버퍼 전체가 아니라
    // 일부만 보낼 수 있어, "어디까지 보냈는지"를 기억해뒀다가 다음 flushPending에서 이어서 보낸다.
    std::size_t writeOffset_;
    std::size_t maxLineLength_;
    std::size_t maxPendingBytes_;
    SendOperation sendOperation_;
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
