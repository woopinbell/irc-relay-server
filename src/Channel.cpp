#include "Channel.hpp"

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
