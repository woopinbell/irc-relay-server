#include "IrcMessage.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

IrcMessage::IrcMessage() {
}

IrcMessage::IrcMessage(const std::string& messagePrefix,
                       const std::string& messageCommand,
                       const std::vector<std::string>& messageParams)
    : prefix(messagePrefix),
      command(upper(messageCommand)),
      params(messageParams) {
}

bool IrcMessage::isCommand(const std::string& name) const {
    return command == upper(name);
}

std::string IrcMessage::param(std::size_t index, const std::string& fallback) const {
    if (index >= params.size()) {
        return fallback;
    }
    return params[index];
}

// [INTV:EDGE] IRC 프로토콜은 공백을 포함하거나 빈 값이거나 ':'로 시작하는 파라미터를 마지막 "trailing"
// 파라미터로만 허용한다 — 그런 값을 만나면 ':'를 붙여 "여기부터 끝까지가 한 파라미터"임을 표시한다.
// - [TRAP] 이 마킹을 params 리스트의 아무 위치에서나 독립적으로 적용하면(이 구현처럼), 공백이 섞인
//   값이 마지막이 아닌 자리에 있을 경우 상대방 파서가 뒤에 남은 파라미터들을 trailing의 일부로 잘못
//   해석하는 프로토콜 위반 메시지가 만들어질 수 있다. 호출부가 "공백 포함 값은 항상 마지막 파라미터로만
//   넘긴다"는 불변조건을 지켜야 한다.
std::string IrcMessage::toLine() const {
    std::ostringstream out;
    if (!prefix.empty()) {
        out << ':' << prefix << ' ';
    }
    out << command;
    for (std::size_t i = 0; i < params.size(); ++i) {
        out << ' ';
        const std::string& value = params[i];
        if (value.empty() || value.find(' ') != std::string::npos || value[0] == ':') {
            out << ':' << value;
        } else {
            out << value;
        }
    }
    out << "\r\n";
    return out.str();
}

// [INTV:FLOW] RFC 1459 라인 문법 파싱: [':'prefix SP] command *(SP param) [SP ':'trailing].
// - [FLOW] 1. 선행 ':'가 있으면 prefix 파싱(다음 공백까지) -> 2. 공백 스킵 -> 3. command 토큰 추출 ->
//   4. 남은 부분을 공백 기준으로 반복 파싱하되, 도중 ':'로 시작하는 토큰을 만나면 그 뒤 전부를 마지막
//   파라미터 하나로 흡수하고 종료
// - [TRAP] trailing 파라미터(':' 이후)는 내부에 공백이 있어도 더 이상 분리하면 안 된다. 이 예외 처리
//   (params.push_back(...); break;)를 빼먹으면 "PRIVMSG #chan :hello world"의 메시지 본문이
//   "hello"/"world" 두 파라미터로 잘못 쪼개진다.
bool IrcMessage::parseLine(const std::string& line, IrcMessage& out, std::string* error) {
    const std::string trimmed = trimFrame(line);
    out = IrcMessage();
    out.raw = trimmed;

    if (trimmed.empty()) {
        if (error) {
            *error = "empty IRC frame";
        }
        return false;
    }
    // [INTV:EDGE] RFC가 규정한 최대 프레임 길이(510옥텟, CRLF 제외) — 이 상한이 없으면 파서가 임의로
    // 긴 입력을 계속 처리하려 들 수 있다(Connection의 maxLineLength와는 별개로 프로토콜 계층에서
    // 한 번 더 강제하는 규격 준수 검증).
    if (trimmed.size() > 510) {
        if (error) {
            *error = "IRC frame exceeds 510 octets before CRLF";
        }
        return false;
    }

    std::size_t pos = 0;
    if (trimmed[pos] == ':') {
        const std::size_t end = trimmed.find(' ');
        if (end == std::string::npos || end == 1) {
            if (error) {
                *error = "message prefix is missing a command";
            }
            return false;
        }
        out.prefix = trimmed.substr(1, end - 1);
        pos = end + 1;
    }

    while (pos < trimmed.size() && trimmed[pos] == ' ') {
        ++pos;
    }

    const std::size_t commandStart = pos;
    while (pos < trimmed.size() && trimmed[pos] != ' ') {
        ++pos;
    }
    if (commandStart == pos) {
        if (error) {
            *error = "IRC command is missing";
        }
        return false;
    }
    out.command = upper(trimmed.substr(commandStart, pos - commandStart));

    while (pos < trimmed.size()) {
        while (pos < trimmed.size() && trimmed[pos] == ' ') {
            ++pos;
        }
        if (pos >= trimmed.size()) {
            break;
        }
        if (trimmed[pos] == ':') {
            out.params.push_back(trimmed.substr(pos + 1));
            break;
        }
        const std::size_t paramStart = pos;
        while (pos < trimmed.size() && trimmed[pos] != ' ') {
            ++pos;
        }
        out.params.push_back(trimmed.substr(paramStart, pos - paramStart));
    }

    return true;
}

// [INTV:EDGE] 개행 문자가 여러 개 쌓인 버퍼에서 완성된 메시지를 전부 뽑아내고, 파싱에 실패한 개별
// 프레임은 (에러만 기록한 채) 건너뛰어 나머지 유효한 메시지 처리를 막지 않는다.
// - [TRAP] 파싱 실패 시 즉시 함수 전체를 중단하도록 재구현하면, 한 클라이언트가 깨진 프레임 하나를
//   보낸 것만으로 같은 배치에 있던 이후의 정상 메시지들까지 전부 버려지는 과도한 실패 전파가 된다.
std::vector<IrcMessage> IrcMessage::consumeBuffer(std::string& buffer, std::string* error) {
    std::vector<IrcMessage> messages;
    std::size_t newline = buffer.find('\n');
    while (newline != std::string::npos) {
        const std::string frame = buffer.substr(0, newline + 1);
        buffer.erase(0, newline + 1);

        IrcMessage message;
        std::string localError;
        if (parseLine(frame, message, &localError)) {
            messages.push_back(message);
        } else if (error) {
            *error = localError;
        }

        newline = buffer.find('\n');
    }

    // [INTV:EDGE] 개행 없이 버퍼가 4096바이트를 넘도록 계속 쌓이면(악의적이거나 오동작하는 입력)
    // 더 기다리지 않고 버퍼를 통째로 비운다 — Connection.extractLines의 maxLineLength 방어와 같은
    // 취지를 프로토콜 계층에서 다시 한번 적용한 이중 방어선.
    if (buffer.size() > 4096) {
        if (error) {
            *error = "discarded oversized partial IRC frame";
        }
        buffer.clear();
    }

    return messages;
}

std::string IrcMessage::upper(const std::string& value) {
    std::string copy = value;
    std::transform(copy.begin(), copy.end(), copy.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return copy;
}

std::string IrcMessage::trimFrame(const std::string& line) {
    std::string trimmed = line;
    while (!trimmed.empty() && (trimmed[trimmed.size() - 1] == '\n' || trimmed[trimmed.size() - 1] == '\r')) {
        trimmed.erase(trimmed.size() - 1);
    }
    return trimmed;
}
