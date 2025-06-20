#include "ClientRegistry.hpp"

ClientState::ClientState()
    : fd(-1),
      passOk(false),
      hasNick(false),
      hasUser(false),
      registered(false),
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

void ClientRegistry::erase(int fd) {
    std::map<int, ClientState>::iterator it = _states.find(fd);
    if (it == _states.end()) {
        return;
    }
    _states.erase(it);
}
