#include "IrcApplication.hpp"

#include "IrcMessage.hpp"

#include <vector>

void IrcApplication::handlePass(int fd, const IrcMessage& message) {
    ClientState& client = _clients.state(fd);
    if (client.registered || client.passOk) {
        sendNumeric(fd, 462, std::vector<std::string>(), "You may not reregister");
        return;
    }
    if (message.params.empty()) {
        sendNumeric(fd, 461, std::vector<std::string>(1, "PASS"), "Not enough parameters");
        return;
    }
    if (!_password.empty() && message.params[0] != _password) {
        sendNumeric(fd, 464, std::vector<std::string>(), "Password incorrect");
        requestClose(fd, "Password incorrect");
        return;
    }
    client.passOk = true;
    maybeRegister(fd);
}
