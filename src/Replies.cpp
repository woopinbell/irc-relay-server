#include "Replies.hpp"

#include <iomanip>
#include <sstream>

namespace {
    // [INTV:EDGE] IRC 파라미터 중 공백을 포함하거나, 비어있거나, ':'로 시작하는 값은 trailing 파라미터
    // 표시(':')를 붙여야 파싱 가능한 형태로 직렬화된다(IrcMessage::toLine의 동일 규칙과 대응).
    bool needsTrailingMarker(const std::string& value) {
        return value.empty() || value.find(' ') != std::string::npos || value[0] == ':';
    }
}

std::string Replies::code(int numeric) {
    std::ostringstream out;
    out << std::setfill('0') << std::setw(3) << numeric;
    return out.str();
}

std::string Replies::formatMessage(const std::string& prefix,
                                   const std::string& command,
                                   const std::vector<std::string>& params) {
    std::ostringstream out;
    if (!prefix.empty()) {
        out << ':' << prefix << ' ';
    }
    out << command;
    // [INTV:TRADE_OFF] IrcMessage::toLine()과 달리, 여기서는 needsTrailingMarker를 "마지막 파라미터
    // 자리(i+1==params.size())에서만" 적용한다 — 프로토콜상 trailing은 실제로 마지막 파라미터에만
    // 있을 수 있으므로 이쪽이 더 정확한 구현이다(비교: IrcMessage.cpp toLine()의 TRAP 주석 참고).
    for (std::size_t i = 0; i < params.size(); ++i) {
        out << ' ';
        if (i + 1 == params.size() && needsTrailingMarker(params[i])) {
            out << ':' << params[i];
        } else {
            out << params[i];
        }
    }
    out << "\r\n";
    return out.str();
}

std::string Replies::numeric(const std::string& serverName,
                             const std::string& target,
                             int numericCode,
                             const std::vector<std::string>& params,
                             const std::string& trailing) {
    std::vector<std::string> allParams;
    allParams.push_back(target.empty() ? "*" : target);
    allParams.insert(allParams.end(), params.begin(), params.end());
    allParams.push_back(trailing);
    return formatMessage(serverName, code(numericCode), allParams);
}

std::string Replies::error(const std::string& message) {
    return formatMessage("", "ERROR", std::vector<std::string>(1, message));
}

std::string Replies::hostmask(const std::string& nick, const std::string& user, const std::string& host) {
    const std::string safeUser = user.empty() ? "unknown" : user;
    const std::string safeHost = host.empty() ? "localhost" : host;
    return nick + "!" + safeUser + "@" + safeHost;
}
