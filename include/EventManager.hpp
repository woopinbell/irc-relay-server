#ifndef IRC_EVENT_MANAGER_HPP
#define IRC_EVENT_MANAGER_HPP

#include <memory>
#include <vector>

namespace irc {

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

struct Event {
    int fd = -1;
    EventInterest interests = EventInterest::None;
    bool error = false;
    bool hangup = false;
    int errorCode = 0;
};

class EventManager {
public:
    virtual ~EventManager() = default;

    static std::unique_ptr<EventManager> createDefault();

    virtual void addFd(int fd, EventInterest interests) = 0;
    virtual void updateFd(int fd, EventInterest interests) = 0;
    virtual void removeFd(int fd) = 0;
    virtual std::vector<Event> wait(int timeoutMs) = 0;
};

} // namespace irc

using Event = irc::Event;
using EventInterest = irc::EventInterest;
using EventManager = irc::EventManager;

#endif // IRC_EVENT_MANAGER_HPP
