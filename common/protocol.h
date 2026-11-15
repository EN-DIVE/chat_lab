// protocol.h —— 聊天程序的传输协议底座
//
// 约定：一条消息 = 一行 UTF-8 文本 + '\n'，字段之间用 '|' 分隔。
// TCP 是字节流，没有“消息边界”，所以收到什么都不能假设“一次 recv = 一条消息”。
// LineBuffer 负责把字节流切成一条条完整的行；sendLine 负责保证整行写完。
#pragma once

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <string>
#include <vector>

namespace chat {

// 单行上限：超过说明对方没按协议发送（或者根本不是我们的客户端），直接当错误处理
inline constexpr std::size_t kMaxLine = 4096;

// sendall()函数；send()可能只写出去一部分（发送缓冲区剩多少就只能send多少），所以要循环直到写完
inline bool sendAll(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;  // 被信号打断，重试
            return false;
        }
        if (n == 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// sendline()；封装消息，按协议发一条消息：自动补 '\n'
inline bool sendLine(int fd, const std::string& line) {
    return sendAll(fd, line + "\n");
}

// 字节流->完整行。喂进来的数据可能包含半行、多行、空行，都按字节处理。
// 按字节找 '\n' 是安全的：UTF-8 的多字节字符里不会出现 0x0A，
// 所以即使一个汉字被 recv 切成两半，下一块拼回来也不会乱码。
// linebuffer()；在接收端，缓存recv接收到的字节、找到/n、切出完整行
class LineBuffer {
public:
    // 返回本次凑齐的所有完整行（不含 '\n'；行尾 '\r' 也去掉，兼容 CRLF）
    std::vector<std::string> feed(const char* data, std::size_t n) {
        buf_.append(data, n);//data--recv()拿到的字节数组；n--recv本次读到的字节数；新数据接在buf后面

        std::vector<std::string> lines;// lines数组，存放切好句子
        std::size_t pos;// “\n”位置
        while ((pos = buf_.find('\n')) != std::string::npos) {
            std::string line = buf_.substr(0, pos);//找到"\n"，就把前面这一段切成一句
            if (!line.empty() && line.back() == '\r') line.pop_back();//Windows 换行是 `\r\n`，把刚切的一句末尾\r切掉
            lines.push_back(std::move(line));
            buf_.erase(0, pos + 1);  
        }//循环切直到找不到\n

        if (buf_.size() > kMaxLine) {
            throw std::length_error("单行超过 kMaxLine，对方可能没按协议发送");
        }
        return lines;
    }

    // 缓冲区里还剩多少个字节的“半行”
    std::size_t pending() const { return buf_.size(); }//查看现在缓冲区里还有多少字节的半截数据。

private:
    std::string buf_;//缓存recv字节
};

// split();解析行协议，按分隔符切字段："PRIV|bob|hi" -> {"PRIV","bob","hi"}，在linebuffer()返回之后调用
inline std::vector<std::string> split(const std::string& line, char sep) { //sep--"|"
    std::vector<std::string> out;//存放分隔字段
    std::size_t start = 0;
    while (true) {
        std::size_t pos = line.find(sep, start);
        if (pos == std::string::npos) {//后面没有"|"了
            out.push_back(line.substr(start));//从 start 一直截取到字符串末尾。
            return out;
        }
        out.push_back(line.substr(start, pos - start));//起点start，长度pos-start
        start = pos + 1;
    }
}

}  // namespace chat
