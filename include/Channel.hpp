#ifndef CHANNEL_HPP
#define CHANNEL_HPP

#include <set>
#include <string>
#include <ctime>
#include <vector>

// [INTV:ARCH] 멤버/오퍼레이터를 std::set<int>(clientId)로 표현 — 순서 없는 중복 없는 집합이면 충분하고,
// hasMember/isOperator가 O(log n) 조회면 되는 규모라 std::set이 std::unordered_set보다 굳이 빠를 필요는
// 없지만 반복 순서가 안정적이라는 부가 이점이 있다.
class Channel {
public:
    Channel();
    explicit Channel(const std::string& name);

    const std::string& name() const;
    bool empty() const;
    std::size_t memberCount() const;
    std::time_t createdAt() const;

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
    // [INTV:EDGE] IRC 닉네임은 대소문자를 구분하지 않는 프로토콜 — invite 목록 조회/등록이 항상 이
    // 정규화를 거치도록 강제해야, "Alice"로 초대했는데 "alice"로 join할 때 초대가 인식 안 되는 버그를 막는다.
    static std::string canonicalNick(const std::string& nickname);

private:
    std::string _name;
    std::set<int> _members;
    std::set<int> _operators;
    // [INTV:ARCH] 초대 목록은 canonicalNick으로 정규화된(소문자) 닉네임을 키로 저장 — invite()/isInvited()/
    // clearInvite() 세 곳 모두 저장·조회 시점에 반드시 같은 정규화를 거쳐야 일관성이 유지된다.
    std::set<std::string> _invited;
    std::time_t _createdAt;
    bool _inviteOnly;
    bool _topicProtected;
    bool _hasTopic;
    std::string _topic;
};

#endif
