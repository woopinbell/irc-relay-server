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

int socketErrorFor(int fd)
{
    int error = 0;
    socklen_t length = sizeof(error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == -1) {
        return errno;
    }
    return error;
}

class EpollEventManager : public EventManager {
public:
    EpollEventManager()
        : epollFd_(::epoll_create1(EPOLL_CLOEXEC))
    {
        if (epollFd_ == -1) {
            throw std::system_error(errno, std::generic_category(), "epoll_create1");
        }
    }

    ~EpollEventManager()
    {
        if (epollFd_ != -1) {
            ::close(epollFd_);
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
            if (errno != ENOENT && errno != EBADF) {
                throw std::system_error(errno, std::generic_category(), "epoll_ctl del");
            }
        }
        interests_.erase(found);
    }

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
