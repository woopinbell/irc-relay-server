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

void IrcApplication::handleMessage(int fd, const IrcMessage& message) {
    if (message.command == "PASS") {
        handlePass(fd, message);
    } else if (message.command == "NICK") {
        handleNick(fd, message);
    } else if (message.command == "USER") {
        handleUser(fd, message);
    } else if (message.command == "PING") {
        handlePing(fd, message);
    } else if (message.command == "QUIT") {
        handleQuit(fd, message);
    } else if (!_clients.state(fd).registered) {
        sendNumeric(fd, 451, std::vector<std::string>(), "You have not registered");
    } else if (message.command == "PRIVMSG") {
        handlePrivmsg(fd, message);
    } else if (message.command == "JOIN") {
        handleJoin(fd, message);
    } else if (message.command == "PART") {
        handlePart(fd, message);
    } else if (message.command == "TOPIC") {
        handleTopic(fd, message);
    } else if (message.command == "KICK") {
        handleKick(fd, message);
    } else {
        sendNumeric(fd, 421, std::vector<std::string>(1, message.command), "Unknown command");
    }
}
