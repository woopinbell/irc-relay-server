#ifndef IRC_CONNECTION_LIMITS_HPP
#define IRC_CONNECTION_LIMITS_HPP

#include <cstddef>

namespace irc {
namespace detail {

inline bool canAppendPending(std::size_t pending,
                             std::size_t byteCount,
                             std::size_t limit) noexcept
{
    return pending <= limit && byteCount <= limit - pending;
}

} // namespace detail
} // namespace irc

#endif // IRC_CONNECTION_LIMITS_HPP
