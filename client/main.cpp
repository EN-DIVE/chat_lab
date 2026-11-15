// client.cpp —— 第 1 块：发一行、等一条回包、打印、退出
// 目的不是做聊天，而是确认服务端的拆行 + 回包这条路是通的。
//
// 编译：g++ -std=c++17 -Wall -Wextra client.cpp -o client
// 运行：./client [IP] [端口]           从键盘读一行发送
//       ./client 127.0.0.1 8888 你好    直接把参数当消息发
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

int main(int argc, char** argv) {
    const std::string host = (argc > 1) ? argv[1] : "127.0.0.1";
    const int port = (argc > 2) ? std::stoi(argv[2]) : 8888;

    std::string text;
    if (argc > 3) {
        text = argv[3];
    } else {
        std::cout << "输入要发送的一行: " << std::flush;
        if (!std::getline(std::cin, text)) { //用getline输入
            std::cerr << "没有读到输入\n";
            return 1;
        }
    }

    // ---- 连接（与 3.2 相同）----
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "socket: " << std::strerror(errno) << "\n";
        return 1;
    }
    sockaddr_in addr{};
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

    // ---- 按协议发一行 ----
    if (!chat::sendLine(fd, text)) {
        std::cerr << "发送失败\n";
        ::close(fd);
        return 1;
    }
    std::cout << "已发送: " << text << "\n";

    // ---- 等服务端回包：同样要用 LineBuffer 拆行，不能假设一次 recv 就是一条 ----
    chat::LineBuffer lines;
    char buf[4096];
    while (true) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::cerr << "recv: " << std::strerror(errno) << "\n";
            break;
        }
        if (n == 0) {
            std::cout << "服务端关闭了连接\n";
            break;
        }
        std::vector<std::string> got;
        try {
            got = lines.feed(buf, static_cast<std::size_t>(n));
        } catch (const std::length_error& e) {
            std::cerr << "协议错误: " << e.what() << "\n";
            break;
        }
        if (!got.empty()) {
            std::cout << "服务端回复: " << got.front() << "\n";  // 本步只取第一条
            break;
        }
    }

    ::close(fd);
    return 0;
}
