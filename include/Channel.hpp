#ifndef CHANNEL_HPP
#define CHANNEL_HPP

#include <set>
#include <string>
#include <vector>

class Channel {
public:
    Channel();
    explicit Channel(const std::string& name);

    const std::string& name() const;
    bool empty() const;

    bool hasMember(int clientId) const;
    void addMember(int clientId, bool asOperator);
    void removeMember(int clientId);
    std::vector<int> members() const;

    bool isOperator(int clientId) const;
    void setOperator(int clientId, bool enabled);

    bool isInviteOnly() const;
    void setInviteOnly(bool enabled);

    bool isTopicProtected() const;
    void setTopicProtected(bool enabled);

    bool hasTopic() const;
    const std::string& topic() const;
    void setTopic(const std::string& topic);
    void clearTopic();

    void invite(const std::string& nickname);
    bool isInvited(const std::string& nickname) const;
    void clearInvite(const std::string& nickname);

    std::string modeString() const;

    static bool isValidName(const std::string& name);
    static std::string canonicalNick(const std::string& nickname);

private:
    std::string _name;
    std::set<int> _members;
    std::set<int> _operators;
    std::set<std::string> _invited;
    bool _inviteOnly;
    bool _topicProtected;
    bool _hasTopic;
    std::string _topic;
};

#endif
