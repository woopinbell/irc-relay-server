#ifndef IRC_CLIENT_REGISTRY_HPP
#define IRC_CLIENT_REGISTRY_HPP

#include <chrono>
#include <deque>
#include <map>
#include <string>
#include <vector>

// [INTV:EDGE] steady_clock(단조 시계)을 타이머 로직(레이트 리밋, 타임아웃) 전용으로 사용 — system_clock과
// 달리 NTP 보정이나 사용자의 시스템 시간 변경으로 시간이 거꾸로 흐르지 않는다는 보장이 있다.
// - [TRAP] system_clock으로 재구현하면, 관리자가 시스템 시계를 되돌리는 순간 경과 시간 계산(now - past)이
//   음수가 되어 타임아웃/레이트리밋 로직이 오작동할 수 있다.
typedef std::chrono::steady_clock MonotonicClock;
typedef MonotonicClock::time_point MonotonicTime;

struct ClientState {
    int fd;
    bool passOk;
    bool hasNick;
    bool hasUser;
    bool registered;
    bool awaitingPong;
    std::string nick;
    std::string user;
    std::string realname;
    std::string host;
    std::string pendingPongToken;
    MonotonicTime connectedAt;
    MonotonicTime lastActivityAt;
    MonotonicTime lastPingAt;
    // [INTV:PERF] 슬라이딩 윈도우 레이트 리밋을 위한 명령 타임스탬프 큐 — 앞쪽(오래된 것)만 pop하고
    // 뒤쪽에 push하는 접근이라 std::deque가 std::vector보다 적합하다(앞쪽 제거가 O(1)).
    std::deque<MonotonicTime> commandWindow;

    ClientState();
};

// [INTV:ARCH] fd -> ClientState(map)와 닉네임 -> fd(_nicknameIndex) 두 개의 인덱스를 함께 유지하는
// 이중 인덱스 구조 — "이 fd의 상태는?"과 "이 닉네임을 쓰는 fd는?" 양쪽 조회를 모두 O(log n)에 지원한다.
// - [TRAP] 두 맵은 서로의 정합성을 스스로 보장해주지 않는다. setNickname()/erase()에서 한쪽만 갱신하고
//   다른 쪽을 빼먹으면(특히 개명 시 "옛 닉네임 인덱스 삭제"), 이미 없는 클라이언트의 닉네임이 여전히
//   findFdByNickname()에 잡히는 유령 인덱스가 남는다.
class ClientRegistry {
public:
    ClientState& state(int fd);
    ClientState* find(int fd);
    const ClientState* find(int fd) const;
    bool contains(int fd) const;
    std::vector<int> fds() const;
    int findFdByNickname(const std::string& nickname) const;
    void setNickname(int fd, const std::string& nickname);
    void erase(int fd);

private:
    std::map<int, ClientState> _states;
    std::map<std::string, int> _nicknameIndex;
};

#endif // IRC_CLIENT_REGISTRY_HPP
