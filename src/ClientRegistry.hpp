#ifndef IRC_CLIENT_REGISTRY_HPP
#define IRC_CLIENT_REGISTRY_HPP

#include <chrono>
#include <deque>
#include <map>
#include <string>
#include <vector>

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
    std::deque<MonotonicTime> commandWindow;

    ClientState();
};

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
