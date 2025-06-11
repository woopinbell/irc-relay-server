#if defined(__linux__)

#include "EventManager.hpp"

#include <sys/epoll.h>
#include <unistd.h>

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
} // namespace irc

#endif
