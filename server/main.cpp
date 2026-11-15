// server.cpp —— 第 1 块：仍然只服务一个连接，但改用 LineBuffer 逐行处理并回显
// 本步故意不加线程、不加登录，只验证“字节流 -> 完整行”这一层是否可靠。
//
// 编译：g++ -std=c++17 -Wall -Wextra server.cpp -o server
// 运行：./server [端口]        默认 8888
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
    const int port = (argc > 1) ? std::stoi(argv[1]) : 8888;

    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "socket: " << std::strerror(errno) << "\n";
        return 1;
    }

    int on = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "bind: " << std::strerror(errno) << "（端口被占用？）\n";
        ::close(listen_fd);
        return 1;
    }
    if (::listen(listen_fd, 8) < 0) {
        std::cerr << "listen: " << std::strerror(errno) << "\n";
        ::close(listen_fd);
        return 1;
    }
    std::cout << "服务端已监听 0.0.0.0:" << port << "，等待连接...\n" << std::flush;

    sockaddr_in peer{};
    socklen_t peer_len = sizeof(peer);
    int conn_fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len);
    if (conn_fd < 0) {
        std::cerr << "accept: " << std::strerror(errno) << "\n";
        ::close(listen_fd);
        return 1;
    }
    char ip[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    std::cout << "客户端已连接: " << ip << ":" << ntohs(peer.sin_port) << "\n";

    // ---- 从这里开始是新内容：recv只负责往LineBuffer里灌字节 ----
    chat::LineBuffer lines;
    int line_no = 0;
    char buf[4096];

    while (true) {
        ssize_t n = ::recv(conn_fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::cerr << "recv: " << std::strerror(errno) << "\n";
            break;
        }
        if (n == 0) {  // 对端关闭
            std::cout << "对端关闭了连接\n";
            break;
        }

        std::vector<std::string> got;//装着这次从字节流里切出来的所有完整行消息
        try {
            got = lines.feed(buf, static_cast<std::size_t>(n));//got去获取切开的一段段命令
        } catch (const std::length_error& e) {
            std::cerr << "协议错误: " << e.what() << "\n";
            chat::sendLine(conn_fd, "ERR|LINE_TOO_LONG|单行过长");
            break;
        }

        // 一次 recv 可能拆出 0 行、1 行或好几行；这里逐行处理
        for (const std::string& line : got) {
            if (line.empty()) continue;  // 空行直接忽略
            ++line_no;// 标记这是第几条消息
            std::cout << "[第 " << line_no << " 行] " << line.size() << " 字节: " << line
                      << std::endl;
            if (!chat::sendLine(conn_fd,"ECHO|" + std::to_string(line_no) + "|" + line)) {//组装echo回传报文，向client发送回包
                std::cerr << "回包发送失败，连接可能已断开\n";
                ::close(conn_fd);
                ::close(listen_fd);
                return 1;
            }
        }
        if (lines.pending() > 0) {
            std::cout << "（缓冲区里还留着半行 " << lines.pending() << " 字节，等后续数据）\n";
        }
    }

    ::close(conn_fd);
    ::close(listen_fd);
    return 0;
}
