#ifndef IRC_CONNECTION_LIMITS_HPP
#define IRC_CONNECTION_LIMITS_HPP

#include <cstddef>

namespace irc {
namespace detail {

// [INTV:EDGE] "pending + byteCount <= limit"처럼 더한 뒤 비교하면, size_t(부호 없는 정수)가 오버플로될
// 때 값이 작게 wrap-around 되어 실제로는 한도를 초과했는데도 통과해버리는 버그가 생길 수 있다. 그래서
// 뺄셈 방향("limit - pending")으로 바꿔 오버플로 없이 안전하게 비교한다.
// - [TRAP] pending <= limit 선행 검사를 빼먹으면, pending이 limit보다 큰 비정상 상태에서 limit - pending
//   자체가 부호 없는 정수 뺄셈으로 wrap-around 되어 거대한 값이 나오고, 이어지는 byteCount 비교가
//   무의미해진다. 뺄셈 전에 반드시 "빼는 값이 빼지는 값보다 작거나 같은지" 확인할 것.
inline bool canAppendPending(std::size_t pending,
                             std::size_t byteCount,
                             std::size_t limit) noexcept
{
    return pending <= limit && byteCount <= limit - pending;
}

} // namespace detail
} // namespace irc

#endif // IRC_CONNECTION_LIMITS_HPP
