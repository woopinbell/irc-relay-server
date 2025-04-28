#ifndef IRC_MESSAGE_HPP
#define IRC_MESSAGE_HPP

#include <string>
#include <vector>

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
    static std::vector<IrcMessage> consumeBuffer(std::string& buffer, std::string* error);
    static std::string upper(const std::string& value);
    static std::string trimFrame(const std::string& line);
};

#endif
