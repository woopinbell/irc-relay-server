// [INTV:ARCH] 전처리기 조건부 컴파일 — 이 파일 전체는 리눅스에서만 컴파일 대상에 포함된다. 리눅스가
// 아닌 플랫폼에서는 이 블록이 통째로 빠지고, KqueueEventManager.cpp가 같은 팩토리 함수
// (EventManager::createDefault)를 구현해 그 자리를 채운다.
#if defined(__linux__)

#include "EventManager.hpp"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <system_error>
#include <unordered_map>

namespace irc {
namespace {

// [INTV:EDGE] 우리 쪽 추상 인터레스트(Read/Write)를 epoll 고유 비트 플래그로 변환. EPOLLERR/EPOLLHUP은
// 항상 켜둔다 — 에러/연결종료는 Read/Write 관심 여부와 무관하게 항상 통지받아야 하는 소켓 상태이기
// 때문이다. EPOLLRDHUP(half-close 감지)은 커널 버전에 따라 없을 수 있어 #ifdef로 존재 여부를 확인한다.
int nativeEventsFor(EventInterest interests)
{
    int events = EPOLLERR | EPOLLHUP;
    if (hasInterest(interests, EventInterest::Read)) {
        events |= EPOLLIN;
#ifdef EPOLLRDHUP
        events |= EPOLLRDHUP;
#endif
    }
    if (hasInterest(interests, EventInterest::Write)) {
        events |= EPOLLOUT;
    }
    return events;
}

// [INTV:EDGE] epoll은 "이 fd에 에러가 났다"고만 알려줄 뿐 원인은 알려주지 않으므로, SO_ERROR 소켓
// 옵션을 조회해 커널이 기록해둔 마지막 소켓 에러 코드를 꺼내온다.
int socketErrorFor(int fd)
{
    int error = 0;
    socklen_t length = sizeof(error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == -1) {
        return errno;
    }
    return error;
}

// EventManager 인터페이스를 리눅스 epoll로 구현하는 클래스.
class EpollEventManager : public EventManager {
public:
    // [INTV:EDGE] EPOLL_CLOEXEC: 이 프로세스가 fork+exec로 다른 프로그램을 실행할 때 이 fd가 자식에게
    // 상속되지 않도록 막는 close-on-exec 설정 — fd 누수 방지.
    EpollEventManager()
        : epollFd_(::epoll_create1(EPOLL_CLOEXEC))
    {
        if (epollFd_ == -1) {
            throw std::system_error(errno, std::generic_category(), "epoll_create1");
        }
    }

    // [INTV:ARCH] 소멸자에서 epoll fd를 닫는 RAII — 이 객체가 살아있는 동안 커널 리소스를 자동 관리한다.
    ~EpollEventManager()
    {
        if (epollFd_ != -1) {
            ::close(epollFd_);
        }
    }

    // [INTV:ARCH] interests_ 맵으로 "현재 epoll에 어떤 관심사가 등록돼 있는지"를 직접 캐싱 — epoll_ctl은
    // ADD/MOD/DEL을 구분해서 호출해야 하므로, 이미 등록된 fd인지 판단하려면 이 캐시가 필요하다.
    void addFd(int fd, EventInterest interests) override
    {
        if (interests == EventInterest::None) {
            return;
        }
        if (interests_.find(fd) != interests_.end()) {
            updateFd(fd, interests);
            return;
        }

        control(fd, EPOLL_CTL_ADD, interests);
        interests_[fd] = interests;
    }

    void updateFd(int fd, EventInterest interests) override
    {
        if (interests == EventInterest::None) {
            removeFd(fd);
            return;
        }

        const std::unordered_map<int, EventInterest>::iterator found = interests_.find(fd);
        if (found == interests_.end()) {
            addFd(fd, interests);
            return;
        }

        control(fd, EPOLL_CTL_MOD, interests);
        found->second = interests;
    }

    void removeFd(int fd) override
    {
        const std::unordered_map<int, EventInterest>::iterator found = interests_.find(fd);
        if (found == interests_.end()) {
            return;
        }

        struct epoll_event event;
        event.events = 0;
        event.data.fd = fd;
        if (::epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, &event) == -1) {
            // [INTV:EDGE] ENOENT/EBADF는 fd가 이미 닫혔거나 등록에서 빠진 경우 — 소켓이 close()로
            // 먼저 사라진 뒤 뒤늦게 정리하는 레이스가 흔하므로 에러로 취급하지 않고 조용히 넘어간다.
            if (errno != ENOENT && errno != EBADF) {
                throw std::system_error(errno, std::generic_category(), "epoll_ctl del");
            }
        }
        interests_.erase(found);
    }

    // [INTV:PERF] 리액터의 심장부. std::array<..., 128>로 스택에 고정 크기 배치 버퍼를 잡아, 한 번의
    // 대기에서 최대 128개까지 이벤트를 힙 할당 없이 받아온다.
    // - [FLOW] 1. epoll_wait로 블로킹 대기 -> 2. EINTR이면 빈 결과 반환(호출부가 재시도) -> 3. 나머지
    //   에러는 예외로 전파 -> 4. 성공 시 각 native 이벤트를 플랫폼 독립적 Event로 변환
    // - [TRAP] EINTR을 에러로 처리해 예외를 던지면, 시그널이 자주 오는 환경(디버거 attach, 타이머 등)에서
    //   정상적인 이벤트 루프가 불필요하게 죽는다. EINTR은 "이번 라운드만 빈 결과로 넘기고 재시도"가 표준.
    std::vector<Event> wait(int timeoutMs) override
    {
        std::array<struct epoll_event, 128> nativeEvents;
        const int count =
            ::epoll_wait(epollFd_, nativeEvents.data(), nativeEvents.size(), timeoutMs);

        if (count == -1) {
            if (errno == EINTR) {
                return std::vector<Event>();
            }
            throw std::system_error(errno, std::generic_category(), "epoll_wait");
        }

        std::vector<Event> events;
        events.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            const uint32_t native = nativeEvents[static_cast<std::size_t>(i)].events;

            Event event;
            event.fd = nativeEvents[static_cast<std::size_t>(i)].data.fd;
            event.error = (native & EPOLLERR) != 0;
            event.hangup = (native & EPOLLHUP) != 0;
#ifdef EPOLLRDHUP
            event.hangup = event.hangup || ((native & EPOLLRDHUP) != 0);
#endif
            if (event.error) {
                event.errorCode = socketErrorFor(event.fd);
            }
            if ((native & EPOLLIN) != 0) {
                event.interests |= EventInterest::Read;
            }
            if ((native & EPOLLOUT) != 0) {
                event.interests |= EventInterest::Write;
            }

            events.push_back(event);
        }
        return events;
    }

private:
    int epollFd_;
    std::unordered_map<int, EventInterest> interests_;

    void control(int fd, int operation, EventInterest interests)
    {
        struct epoll_event event;
        event.events = static_cast<uint32_t>(nativeEventsFor(interests));
        event.data.fd = fd;

        if (::epoll_ctl(epollFd_, operation, fd, &event) == -1) {
            throw std::system_error(errno, std::generic_category(), "epoll_ctl");
        }
    }
};

} // namespace

std::unique_ptr<EventManager> EventManager::createDefault()
{
    return std::unique_ptr<EventManager>(new EpollEventManager());
}

} // namespace irc

#endif
