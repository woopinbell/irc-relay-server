#include "Channel.hpp"

#include <algorithm>
#include <cctype>

Channel::Channel()
    : _inviteOnly(false),
      _topicProtected(true),
      _hasTopic(false) {
}

Channel::Channel(const std::string& channelName)
    : _name(channelName),
      _inviteOnly(false),
      _topicProtected(true),
      _hasTopic(false) {
}

const std::string& Channel::name() const {
    return _name;
}

bool Channel::empty() const {
    return _members.empty();
}

bool Channel::hasMember(int clientId) const {
    return _members.find(clientId) != _members.end();
}

void Channel::addMember(int clientId, bool asOperator) {
    _members.insert(clientId);
    if (asOperator) {
        _operators.insert(clientId);
    }
}

void Channel::removeMember(int clientId) {
    _members.erase(clientId);
    _operators.erase(clientId);
}

std::vector<int> Channel::members() const {
    return std::vector<int>(_members.begin(), _members.end());
}

bool Channel::isOperator(int clientId) const {
    return _operators.find(clientId) != _operators.end();
}

void Channel::setOperator(int clientId, bool enabled) {
    if (!_members.count(clientId)) {
        return;
    }
    if (enabled) {
        _operators.insert(clientId);
    } else {
        _operators.erase(clientId);
    }
}

bool Channel::isInviteOnly() const {
    return _inviteOnly;
}

void Channel::setInviteOnly(bool enabled) {
    _inviteOnly = enabled;
}

bool Channel::isTopicProtected() const {
    return _topicProtected;
}

void Channel::setTopicProtected(bool enabled) {
    _topicProtected = enabled;
}

bool Channel::hasTopic() const {
    return _hasTopic;
}

const std::string& Channel::topic() const {
    return _topic;
}

void Channel::setTopic(const std::string& topicValue) {
    _topic = topicValue;
    _hasTopic = true;
}

void Channel::clearTopic() {
    _topic.clear();
    _hasTopic = false;
}

void Channel::invite(const std::string& nickname) {
    _invited.insert(canonicalNick(nickname));
}

bool Channel::isInvited(const std::string& nickname) const {
    return _invited.find(canonicalNick(nickname)) != _invited.end();
}

void Channel::clearInvite(const std::string& nickname) {
    _invited.erase(canonicalNick(nickname));
}

std::string Channel::modeString() const {
    std::string modes = "+";
    if (_inviteOnly) {
        modes += "i";
    }
    if (_topicProtected) {
        modes += "t";
    }
    if (modes == "+") {
        modes += "";
    }
    return modes;
}

bool Channel::isValidName(const std::string& name) {
    if (name.size() < 2 || (name[0] != '#' && name[0] != '&')) {
        return false;
    }
    for (std::size_t i = 0; i < name.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(name[i]);
        if (std::isspace(ch) || ch == ',' || ch == 7) {
            return false;
        }
    }
    return true;
}

std::string Channel::canonicalNick(const std::string& nickname) {
    std::string lowered = nickname;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lowered;
}
