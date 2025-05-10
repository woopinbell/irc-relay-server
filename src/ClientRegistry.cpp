#include "ClientRegistry.hpp"

#include "Channel.hpp"

ClientState::ClientState()
    : fd(-1),
      passOk(false),
      hasNick(false),
      hasUser(false),
      registered(false),
      awaitingPong(false),
      host("localhost"),
      connectedAt(0),
      lastActivityAt(0),
      lastPingAt(0) {
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
