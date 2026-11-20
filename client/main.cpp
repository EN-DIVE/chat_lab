// client.cpp —— 第 2 块：可以连着发多条，方便开两个终端观察服务端是否互不阻塞
//
// 相对第 1 块只改交互方式：连上后循环“读一行 -> 发一行 -> 等一条回包”。
// 仍然是单线程，因为本步服务端只做一问一答；等后面有服务端主动推送的消息时，
// 第 7 块再拆成发送线程 + 接收线程。
//
// 编译：g++ -std=c++17 -Wall -Wextra -pthread client.cpp -o client
// 运行：./client [IP] [端口]
#include "protocol.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

// 读一条完整回包（本步服务端一次只回一行，所以取第一条即可）
bool readOneLine(int fd, chat::LineBuffer& lines, std::string& out) {
    char buf[4096];
    while (true) { 
        std::vector<std::string> got = lines.feed(buf, 0);  // 只扫描，先看缓冲区里有没有现成的
        if (!got.empty()) {
            out = got.front();
            return true;
        }
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0); // 缓冲区空，调用recv去内核拿新字节
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;  // 服务端关闭
        got = lines.feed(buf, static_cast<std::size_t>(n));// 把读到的字节喂给LineBuffer，切分行
        if (!got.empty()) {
            out = got.front();
            return true;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string host = (argc > 1) ? argv[1] : "127.0.0.1";
    const int port = (argc > 2) ? std::stoi(argv[2]) : 8888;

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);//client socket
    if (fd < 0) {
        std::cerr << "socket: " << std::strerror(errno) << "\n";
        return 1;
    }
    sockaddr_in addr{}; //存server地址
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "IP 不合法: " << host << "\n";
        ::close(fd);
        return 1;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "connect: " << std::strerror(errno) << "（服务端启动了吗？）\n";
        ::close(fd);
        return 1;
    }
    std::cout << "已连接 " << host << ":" << port << "，输入一行回车发送，/quit 退出\n";

    chat::LineBuffer lines;
    std::string text;
    while (true) {
        std::cout << "> " << std::flush;//flush强制输出
        if (!std::getline(std::cin, text)) break;  // Ctrl-D强制退出
        if (text.empty()) continue;
        if (text == "/quit") break;

        if (!chat::sendLine(fd, text)) {
            std::cerr << "发送失败\n";
            break;
        }
        std::string reply;
        if (!readOneLine(fd, lines, reply)) { //用readoneline里面的recv()阻塞，不收到server回复就不能继续发
            std::cout << "服务端已断开\n";
            break;
        }
        std::cout << "服务端: " << reply << "\n";
    }

    ::shutdown(fd, SHUT_WR);//shutdown优雅关闭写方向
    ::close(fd);
    std::cout << "已断开\n";
    return 0;
}
