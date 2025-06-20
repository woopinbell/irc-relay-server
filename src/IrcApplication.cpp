#include "IrcApplication.hpp"

#include "Connection.hpp"
#include "IrcMessage.hpp"

IrcApplication::IrcApplication(Server& server, const std::string& password, const RuntimeConfig& runtime)
    : _server(server),
      _password(password),
      _runtime(runtime),
      _serverName("irc.reference.local") {
}

void IrcApplication::onConnect(Connection& connection) {
    ClientState client;
    client.fd = connection.fd();
    client.host = connection.peerAddress();
    client.passOk = _password.empty();
    _clients.state(client.fd) = client;
}

void IrcApplication::onLine(Connection& connection, const std::string& line) {
    const int fd = connection.fd();
    if (!_clients.contains(fd)) {
        onConnect(connection);
    }

    IrcMessage message;
    std::string parseError;
    if (!IrcMessage::parseLine(line, message, &parseError)) {
        sendNumeric(fd, 417, std::vector<std::string>(), parseError);
        return;
    }
    handleMessage(fd, message);
}

void IrcApplication::onDisconnect(Connection& connection, const std::string& reason) {
    removeClientState(connection.fd(), reason, true);
}
