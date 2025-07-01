#ifndef IRC_APPLICATION_HPP
#define IRC_APPLICATION_HPP

#include "Channel.hpp"
#include "ClientRegistry.hpp"
#include "RuntimeConfig.hpp"
#include "Server.hpp"

#include <cstddef>
#include <ctime>
#include <map>
#include <string>
#include <utility>
#include <vector>

class IrcMessage;

struct AppMetrics {
    std::size_t commandsHandled;
    std::size_t messagesRelayed;
    std::size_t roomsCreated;
    std::size_t rateLimitedClients;
    std::size_t idleTimeouts;
    std::size_t heartbeatPings;

    AppMetrics();
};

void logEvent(
    const std::string& eventName,
    const std::vector<std::pair<std::string, std::string> >& fields
);

class IrcApplication {
public:
    IrcApplication(Server& server, const std::string& password, const RuntimeConfig& runtime);

    void onConnect(Connection& connection);
    void onLine(Connection& connection, const std::string& line);
    void onDisconnect(Connection& connection, const std::string& reason);
    void onTick();
    void logMetrics() const;

private:
    Server& _server;
    std::string _password;
    RuntimeConfig _runtime;
    std::string _serverName;
    ClientRegistry _clients;
    std::map<std::string, Channel> _channels;
    AppMetrics _metrics;

    static std::vector<std::string> splitComma(const std::string& value);
    static bool isChannelTarget(const std::string& target);

    void handleMessage(int fd, const IrcMessage& message);
    void maintainClient(int fd, std::time_t now);
    bool recordCommand(int fd, std::time_t now);

    void handlePass(int fd, const IrcMessage& message);
    void handleNick(int fd, const IrcMessage& message);
    void handleUser(int fd, const IrcMessage& message);
    void handlePing(int fd, const IrcMessage& message);
    void handlePong(int fd, const IrcMessage& message);
    void handleQuit(int fd, const IrcMessage& message);
    void maybeRegister(int fd);

    void handlePrivmsg(int fd, const IrcMessage& message);

    void handleJoin(int fd, const IrcMessage& message);
    void handlePart(int fd, const IrcMessage& message);
    void handleTopic(int fd, const IrcMessage& message);
    void handleKick(int fd, const IrcMessage& message);
    void handleInvite(int fd, const IrcMessage& message);
    void handleMode(int fd, const IrcMessage& message);
    void handleList(int fd, const IrcMessage& message);
    void handleNames(int fd, const IrcMessage& message);
    void handleChannelMode(int fd, const IrcMessage& message);

    void handleMetrics(int fd);
    Channel& ensureChannel(const std::string& name);
    Channel* findChannelForCommand(int fd, const std::string& name, bool requireMembership);
    void sendTopicReply(int fd, const Channel& channel);
    void sendNames(int fd, const Channel& channel);
    void partAllChannels(int fd, const std::string& reason);
    void partChannel(int fd, const std::string& channelName, const std::string& reason);
    void broadcastMode(int fd, const Channel& channel, const std::string& mode, const std::string& arg);
    void broadcastToChannel(const std::string& channelName, const std::string& line, int exceptFd);
    void broadcastToCommon(int fd, const std::string& line, bool includeSelf);
    void eraseChannelIfEmpty(const std::string& channelName);
    int findNick(const std::string& nickname) const;
    std::string replyTarget(int fd) const;
    std::string prefixFor(int fd) const;
    std::string prefixFor(const ClientState& client) const;
    void sendNumeric(
        int fd,
        int numericCode,
        const std::vector<std::string>& params,
        const std::string& trailing
    );
    void sendNumericRaw(int fd, int numericCode, const std::vector<std::string>& params);
    void sendRaw(int fd, const std::string& line);
    void requestClose(int fd, const std::string& reason);
    void removeClientState(int fd, const std::string& reason, bool notifyPeers);
};

#endif // IRC_APPLICATION_HPP
