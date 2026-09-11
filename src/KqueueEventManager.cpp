// [INTV:ARCH] EpollEventManager.cpp와 대칭인 전처리기 조건부 컴파일 — 이 파일은 macOS/BSD 계열에서만
// 컴파일되어 EventManager::createDefault()의 kqueue 버전을 제공한다.
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)

#include "EventManager.hpp"

#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <unordered_map>

namespace irc {
namespace {

// [INTV:TRADE_OFF] kqueue는 epoll과 달리 "관심사 하나의 비트마스크"가 아니라 필터(EVFILT_READ/
// EVFILT_WRITE)별로 개별 등록/해제하는 모델이다. 그래서 이 클래스는 Read/Write를 각각 별도 kevent
// 항목으로 다루고, 바뀐 필터만 골라 add/delete 하는 diff 로직(applyInterestChange)이 핵심이 된다 —
// epoll_ctl(MOD) 한 번으로 끝나는 EpollEventManager와의 근본적인 API 설계 차이.
class KqueueEventManager : public EventManager {
public:
    KqueueEventManager()
        : kqueueFd_(::kqueue())
    {
        if (kqueueFd_ == -1) {
            throw std::system_error(errno, std::generic_category(), "kqueue");
        }
    }

    ~KqueueEventManager()
    {
        if (kqueueFd_ != -1) {
            ::close(kqueueFd_);
        }
    }

    void addFd(int fd, EventInterest interests) override
    {
        if (interests == EventInterest::None) {
            return;
        }
        if (interests_.find(fd) != interests_.end()) {
            updateFd(fd, interests);
            return;
        }
        applyInterestChange(fd, EventInterest::None, interests);
        interests_[fd] = interests;
    }

    // [INTV:TRAP] epoll_ctl(MOD)처럼 한 번에 덮어쓰는 API가 kqueue엔 없다 — 이전 interests와 새
    // interests를 직접 비교(diff)해 바뀐 필터만 add/delete 해야 한다. "일단 전부 DELETE 후 다시
    // ADD"로 재구현하면 상태가 그대로인 필터까지 불필요하게 재등록되어 시스템 콜이 늘어난다.
    void updateFd(int fd, EventInterest interests) override
    {
        const std::unordered_map<int, EventInterest>::iterator found = interests_.find(fd);
        const EventInterest oldInterests =
            found == interests_.end() ? EventInterest::None : found->second;

        if (interests == EventInterest::None) {
            removeFd(fd);
            return;
        }

        applyInterestChange(fd, oldInterests, interests);
        interests_[fd] = interests;
    }

    void removeFd(int fd) override
    {
        const std::unordered_map<int, EventInterest>::iterator found = interests_.find(fd);
        if (found == interests_.end()) {
            return;
        }

        removeFilterIfWatched(fd, found->second, EventInterest::Read, EVFILT_READ);
        removeFilterIfWatched(fd, found->second, EventInterest::Write, EVFILT_WRITE);
        interests_.erase(found);
    }

    // [INTV:ARCH] epoll_wait와 같은 역할의 kqueue 대기 함수 — changelist를 NULL/0으로 넘겨 "변경 없이
    // 대기만" 요청하고, eventlist에 준비된 이벤트를 최대 128개까지 받아온다.
    std::vector<Event> wait(int timeoutMs) override
    {
        std::array<struct kevent, 128> nativeEvents;
        struct timespec timeout;
        struct timespec* timeoutPtr = NULL;

        if (timeoutMs >= 0) {
            timeout.tv_sec = timeoutMs / 1000;
            timeout.tv_nsec = (timeoutMs % 1000) * 1000000;
            timeoutPtr = &timeout;
        }

        const int count =
            ::kevent(kqueueFd_, NULL, 0, nativeEvents.data(), nativeEvents.size(), timeoutPtr);
        if (count == -1) {
            if (errno == EINTR) {
                return std::vector<Event>();
            }
            throw std::system_error(errno, std::generic_category(), "kevent wait");
        }

        std::vector<Event> events;
        events.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            // [INTV:EDGE] kqueue 고유 필드를 Event로 변환. EV_EOF는 "상대가 연결을 닫음"을 뜻하는
            // kqueue 플래그(epoll의 EPOLLHUP/EPOLLRDHUP에 대응). data/fflags는 상황에 따라 의미가
            // 바뀌는 kqueue 특유의 필드 — 에러일 땐 에러 코드, EOF일 땐 관련 플래그를 담는다.
            Event event;
            event.fd = static_cast<int>(nativeEvents[static_cast<std::size_t>(i)].ident);
            event.error = (nativeEvents[static_cast<std::size_t>(i)].flags & EV_ERROR) != 0;
            event.hangup = (nativeEvents[static_cast<std::size_t>(i)].flags & EV_EOF) != 0;
            if (event.error) {
                event.errorCode = static_cast<int>(nativeEvents[static_cast<std::size_t>(i)].data);
            } else if (event.hangup) {
                event.errorCode = static_cast<int>(nativeEvents[static_cast<std::size_t>(i)].fflags);
            }

            // [INTV:TRADE_OFF] 어떤 필터(EVFILT_READ/EVFILT_WRITE)에서 온 이벤트인지로 Read/Write
            // 관심사를 되돌린다 — epoll처럼 한 이벤트에 여러 관심사가 한꺼번에 담기지 않고, kqueue는
            // 필터마다 별도 이벤트로 온다(한 fd가 Read/Write 둘 다 준비되면 이 루프에 두 항목이 생긴다).
            if (nativeEvents[static_cast<std::size_t>(i)].filter == EVFILT_READ) {
                event.interests |= EventInterest::Read;
            } else if (nativeEvents[static_cast<std::size_t>(i)].filter == EVFILT_WRITE) {
                event.interests |= EventInterest::Write;
            }

            events.push_back(event);
        }
        return events;
    }

private:
    int kqueueFd_;
    std::unordered_map<int, EventInterest> interests_;

    void applyInterestChange(int fd, EventInterest oldInterests, EventInterest newInterests)
    {
        updateFilterIfChanged(fd, oldInterests, newInterests, EventInterest::Read, EVFILT_READ);
        updateFilterIfChanged(fd, oldInterests, newInterests, EventInterest::Write, EVFILT_WRITE);
    }

    // [INTV:PERF] Read 또는 Write 필터 각각에 대해 "이전엔 없었는데 지금은 있음(등록)" / "이전엔
    // 있었는데 지금은 없음(해제)" 상태 변화만 골라 kevent를 호출한다 — 상태가 그대로면 아무것도 하지
    // 않아 불필요한 시스템 콜을 피한다.
    void updateFilterIfChanged(
        int fd,
        EventInterest oldInterests,
        EventInterest newInterests,
        EventInterest interest,
        int16_t filter)
    {
        const bool hadInterest = hasInterest(oldInterests, interest);
        const bool wantsInterest = hasInterest(newInterests, interest);
        if (hadInterest == wantsInterest) {
            return;
        }

        const uint16_t flags = wantsInterest ? (EV_ADD | EV_ENABLE) : EV_DELETE;
        applyFilterChange(fd, filter, flags, false);
    }

    void removeFilterIfWatched(int fd, EventInterest interests, EventInterest interest, int16_t filter)
    {
        if (hasInterest(interests, interest)) {
            applyFilterChange(fd, filter, EV_DELETE, true);
        }
    }

    // [INTV:ARCH] EV_SET은 kevent 구조체를 채워주는 매크로 — kqueue API의 관용적 사용법. 여기서는
    // kevent()를 "이벤트 대기"가 아니라 "등록 변경 통지" 용도로 호출한다(eventlist를 NULL로 넘겨
    // 결과를 받지 않음 — changelist와 eventlist를 한 호출에 겸용할 수도 있지만 이 코드는 분리해서 쓴다).
    void applyFilterChange(int fd, int16_t filter, uint16_t flags, bool ignoreMissing)
    {
        struct kevent change;
        EV_SET(&change, static_cast<uintptr_t>(fd), filter, flags, 0, 0, NULL);

        if (::kevent(kqueueFd_, &change, 1, NULL, 0, NULL) == -1) {
            // epoll 쪽 EPOLL_CTL_DEL과 같은 이유로, fd가 이미 사라진 경우의 실패는 호출부가 선택적으로
            // 무시할 수 있게 한다.
            if (ignoreMissing && (errno == ENOENT || errno == EBADF)) {
                return;
            }
            throw std::system_error(errno, std::generic_category(), "kevent update");
        }
    }
};

} // namespace

std::unique_ptr<EventManager> EventManager::createDefault()
{
    return std::unique_ptr<EventManager>(new KqueueEventManager());
}

} // namespace irc

#endif
