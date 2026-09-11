#ifndef IRC_EVENT_MANAGER_HPP
#define IRC_EVENT_MANAGER_HPP

#include <memory>
#include <vector>

namespace irc {

// [INTV:ARCH] enum class(스코프드 enum) 값을 1u<<0, 1u<<1로 잡아 서로 다른 비트에 대응시킨 비트마스크
// 플래그. enum class는 int로 암묵 변환되지 않아 타입 안전하지만, 그 대가로 |/& 연산자를 기본 지원하지
// 않는다 — 아래에서 직접 오버로딩해 비트마스크처럼 자연스럽게 합성/조회할 수 있게 만든다.
enum class EventInterest : unsigned int {
    None = 0,
    Read = 1u << 0,
    Write = 1u << 1
};

constexpr EventInterest operator|(EventInterest lhs, EventInterest rhs) noexcept
{
    return static_cast<EventInterest>(
        static_cast<unsigned int>(lhs) | static_cast<unsigned int>(rhs));
}

constexpr EventInterest operator&(EventInterest lhs, EventInterest rhs) noexcept
{
    return static_cast<EventInterest>(
        static_cast<unsigned int>(lhs) & static_cast<unsigned int>(rhs));
}

inline EventInterest& operator|=(EventInterest& lhs, EventInterest rhs) noexcept
{
    lhs = lhs | rhs;
    return lhs;
}

constexpr bool hasInterest(EventInterest interests, EventInterest expected) noexcept
{
    return static_cast<unsigned int>(interests & expected) != 0;
}

// epoll/kqueue 같은 OS 이벤트 대기 API가 "이 fd가 준비됐다"고 알려줄 때 쓰는 결과 한 건을 표현하는,
// 플랫폼 독립적인 구조체.
struct Event {
    int fd = -1;
    EventInterest interests = EventInterest::None;
    bool error = false;
    bool hangup = false;
    int errorCode = 0;
};

// [INTV:ARCH] 리액터(Reactor) 패턴: OS의 이벤트 통지 메커니즘(리눅스 epoll, macOS/BSD kqueue)을 감싸는
// 추상 인터페이스. 순수 가상 함수만 있어 실질적으로 "인터페이스"이고, 실제 구현은
// EpollEventManager/KqueueEventManager가 담당한다.
// - [TRAP] unique_ptr<EventManager>처럼 베이스 클래스 포인터로 삭제될 것을 전제로 하는 타입은 소멸자를
//   반드시 virtual로 선언해야 한다. 빼먹으면 파생 클래스 소멸자가 호출되지 않는 정의되지 않은 동작이 된다.
class EventManager {
public:
    virtual ~EventManager() = default;

    // [INTV:ARCH] [INTV:TRADE_OFF] 정적 팩토리 메서드로 컴파일 타임 플랫폼 분기 — 실제 분기는 이
    // 함수를 정의하는 EpollEventManager.cpp/KqueueEventManager.cpp 양쪽의 #if 전처리기로 이뤄지고,
    // 링커 시점엔 해당 플랫폼의 파일만 컴파일에 포함되어 심볼 충돌 없이 하나만 남는다.
    // - [TRAP] 런타임 if/else로 두 구현을 다 컴파일해두고 분기하려 하면, 리눅스가 아닌 플랫폼에서
    //   <sys/epoll.h>가 없어 컴파일 자체가 실패한다. 플랫폼별 API는 컴파일 타임에 배제해야 한다.
    static std::unique_ptr<EventManager> createDefault();

    virtual void addFd(int fd, EventInterest interests) = 0;
    virtual void updateFd(int fd, EventInterest interests) = 0;
    virtual void removeFd(int fd) = 0;
    // [INTV:FLOW] 리액터의 핵심 연산: 등록된 fd들 중 하나라도 준비될 때까지(또는 timeoutMs만큼)
    // 블로킹하고, 준비된 fd 목록을 반환한다. 호출부(Server)는 이 결과를 순회하며 각 fd에 맞는 처리를
    // 디스패치하는 이벤트 루프를 돈다.
    virtual std::vector<Event> wait(int timeoutMs) = 0;
};

} // namespace irc

using Event = irc::Event;
using EventInterest = irc::EventInterest;
using EventManager = irc::EventManager;

#endif // IRC_EVENT_MANAGER_HPP
