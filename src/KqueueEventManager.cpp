#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)

#include "EventManager.hpp"

#include <sys/event.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>
#include <unordered_map>

namespace irc {
namespace {

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

private:
    int kqueueFd_;
    std::unordered_map<int, EventInterest> interests_;

    void applyInterestChange(int fd, EventInterest oldInterests, EventInterest newInterests)
    {
        updateFilterIfChanged(fd, oldInterests, newInterests, EventInterest::Read, EVFILT_READ);
        updateFilterIfChanged(fd, oldInterests, newInterests, EventInterest::Write, EVFILT_WRITE);
    }

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

    void applyFilterChange(int fd, int16_t filter, uint16_t flags, bool ignoreMissing)
    {
        struct kevent change;
        EV_SET(&change, static_cast<uintptr_t>(fd), filter, flags, 0, 0, NULL);

        if (::kevent(kqueueFd_, &change, 1, NULL, 0, NULL) == -1) {
            if (ignoreMissing && (errno == ENOENT || errno == EBADF)) {
                return;
            }
            throw std::system_error(errno, std::generic_category(), "kevent update");
        }
    }
};

} // namespace
} // namespace irc

#endif
