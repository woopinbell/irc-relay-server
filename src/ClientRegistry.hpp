#ifndef IRC_CLIENT_REGISTRY_HPP
#define IRC_CLIENT_REGISTRY_HPP

#include <map>
#include <string>
#include <vector>

struct ClientState {
    int fd;
    bool passOk;
    bool hasNick;
    bool hasUser;
    bool registered;
    std::string nick;
    std::string user;
    std::string realname;
    std::string host;

    ClientState();
};

class ClientRegistry {
public:
    ClientState& state(int fd);
    ClientState* find(int fd);
    const ClientState* find(int fd) const;
    bool contains(int fd) const;
    std::vector<int> fds() const;
    void erase(int fd);

private:
    std::map<int, ClientState> _states;
};

#endif // IRC_CLIENT_REGISTRY_HPP
