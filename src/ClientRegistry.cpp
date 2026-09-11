#include "ClientRegistry.hpp"

#include "Channel.hpp"

ClientState::ClientState()
    : fd(-1),
      passOk(false),
      hasNick(false),
      hasUser(false),
      registered(false),
      awaitingPong(false),
      host("localhost") {
}

ClientState& ClientRegistry::state(int fd) {
    return _states[fd];
}

ClientState* ClientRegistry::find(int fd) {
    std::map<int, ClientState>::iterator it = _states.find(fd);
    return it == _states.end() ? NULL : &it->second;
}

const ClientState* ClientRegistry::find(int fd) const {
    std::map<int, ClientState>::const_iterator it = _states.find(fd);
    return it == _states.end() ? NULL : &it->second;
}

bool ClientRegistry::contains(int fd) const {
    return _states.find(fd) != _states.end();
}

std::vector<int> ClientRegistry::fds() const {
    std::vector<int> values;
    for (std::map<int, ClientState>::const_iterator it = _states.begin(); it != _states.end(); ++it) {
        values.push_back(it->first);
    }
    return values;
}

int ClientRegistry::findFdByNickname(const std::string& nickname) const {
    const std::map<std::string, int>::const_iterator it =
        _nicknameIndex.find(Channel::canonicalNick(nickname));
    return it == _nicknameIndex.end() ? -1 : it->second;
}

// [INTV:EDGE] 개명 시 반드시 "옛 닉네임 인덱스 제거 -> 새 닉네임 인덱스 등록" 순서를 지켜야 두 맵의
// 정합성이 유지된다.
// - [TRAP] 옛 인덱스 삭제를 빼먹으면, 이 클라이언트가 새 닉네임으로 재등록된 뒤에도 findFdByNickname이
//   옛 닉네임에 대해 여전히 같은 fd를 반환하는 유령 매핑이 남는다.
void ClientRegistry::setNickname(int fd, const std::string& nickname) {
    const std::string canonical = Channel::canonicalNick(nickname);
    ClientState& client = state(fd);
    if (!client.nick.empty()) {
        _nicknameIndex.erase(Channel::canonicalNick(client.nick));
    }
    client.nick = nickname;
    client.hasNick = true;
    _nicknameIndex[canonical] = fd;
}

// [INTV:EDGE] fd를 제거하기 전에 반드시 닉네임 인덱스부터 정리 — 순서를 바꿔 _states.erase를 먼저
// 하면 it->second.nick을 더 이상 읽을 수 없어 어떤 닉네임을 지워야 할지 알 수 없게 된다.
void ClientRegistry::erase(int fd) {
    std::map<int, ClientState>::iterator it = _states.find(fd);
    if (it == _states.end()) {
        return;
    }
    if (!it->second.nick.empty()) {
        _nicknameIndex.erase(Channel::canonicalNick(it->second.nick));
    }
    _states.erase(it);
}
