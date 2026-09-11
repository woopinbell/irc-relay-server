#ifndef IRC_MESSAGE_HPP
#define IRC_MESSAGE_HPP

#include <string>
#include <vector>

// [INTV:ARCH] IRC 프로토콜(RFC 1459 계열) 한 줄 메시지를 표현하는 값 타입: [:prefix] COMMAND
// [params...] [:trailing]. 파싱(parseLine)과 직렬화(toLine)가 대칭을 이루는 왕복 변환 쌍이다.
class IrcMessage {
public:
    std::string prefix;
    std::string command;
    std::vector<std::string> params;
    std::string raw;

    IrcMessage();
    IrcMessage(const std::string& prefix,
               const std::string& command,
               const std::vector<std::string>& params);

    bool isCommand(const std::string& name) const;
    std::string param(std::size_t index, const std::string& fallback = "") const;
    std::string toLine() const;

    static bool parseLine(const std::string& line, IrcMessage& out, std::string* error);
    // [INTV:EDGE] Connection이 이미 개행 기준으로 한 줄씩 잘라주지만, consumeBuffer는 그와 별개로
    // "버퍼 안에 완성된 줄이 여러 개 쌓여 있을 수 있다"는 것과 "완성되지 않은 채 너무 길게 쌓이는 것"
    // 둘 다를 이 계층에서 한 번 더 방어한다 — 전송 계층(Connection)과 프로토콜 계층의 중복 방어.
    static std::vector<IrcMessage> consumeBuffer(std::string& buffer, std::string* error);
    static std::string upper(const std::string& value);
    static std::string trimFrame(const std::string& line);
};

#endif
