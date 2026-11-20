// server.cpp —— 第 2 块：多客户端骨架
//
// 相对第 1 块只改结构，不改协议：
//   1. accept 放进 while 循环，服务端常驻；
//   2. 每个连接用一个 Client 对象（shared_ptr 持有），交给一个 std::thread；
//   3. 每个连接有自己的 LineBuffer、行号、写锁，互不干扰；
//   4. 日志加全局锁，避免多个线程的 cout 输出交错。
// 业务上仍然是“逐行回显”，登录 / 群聊 / 私聊在后续步骤加入。
//
// 编译：g++ -std=c++17 -Wall -Wextra -pthread server.cpp -o server
// 运行：./server [端口]        Ctrl-C 结束
#include "protocol.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// 多个连接线程都会写日志，不加锁会出现两行粘在一起
std::mutex g_log_mu;//globle log mutex全局日志锁

void logMsg(const std::string& msg) { //logMsg 的本质不是 “日志打印函数”，是**多线程安全的打印函数**，锁才是灵魂
    std::lock_guard<std::mutex> lk(g_log_mu);//lock_guard 自动锁，创建函数自动lock，离开大括号自动unlock--让它一旦开始日志函数就先lock锁住，等到打印完成之后出括号再自动解锁
    std::cout << msg << "\n" << std::flush;//flush强行把缓冲区内容刷到终端，实时看到日志
}

std::atomic<int> g_active{0};  // 当前连接数，仅用于日志，统计当前在线客户端数量，g_active.load() // 读取当前值

// 一条连接的上下文。fd 谁创建谁负责关闭：这里交给析构函数，线程结束时自动 close
struct Client { //新建client结构体
    int fd = -1; //socket，//fd 是这条 TCP 连接的句柄，内核早就记住了这条连接的四元组：(本机 IP, 本机端口，对端 IP, 对端端口)；fd 只是给你用户态程序用的一个 “快捷编号”，用来查内核里那张四元组表。
    std::string ip;
    int port = 0; //ip port 是client的地址
    std::mutex wmu;  // 这条连接的写锁：保证整行不被别的线程插进来，可能多个线程同时 send 同一个 socket

    ~Client() {
        if (fd >= 0) ::close(fd);
    }

    std::string tag() const { return ip + ":" + std::to_string(port); }//打印client地址的函数
};

// 所有发给这个客户端的数据统一走这个函数，强制上锁，统一规范。为第 5 块“广播时多个线程写同一连接”提前立好规矩
bool sendTo(Client& c, const std::string& line) {
    std::lock_guard<std::mutex> lk(c.wmu);
    return chat::sendLine(c.fd, line);
}

// 单条连接的收发循环：第 1 块的逻辑原样搬进来，只是行号变成局部变量
void serveOne(Client& c) {
    chat::LineBuffer lines;
    int line_no = 0;
    char buf[4096];

    while (true) {
        ssize_t n = ::recv(c.fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR) continue; //被信号打断，直接重试就好
            if (errno == ECONNRESET) {  // 对端强制重置连接如kill -9，这条socket废掉
                logMsg("连接被重置: " + c.tag());
            } else {
                logMsg("recv 失败（" + c.tag() + "）: " + std::strerror(errno));
            }
            return;
        }
        if (n == 0) {
            logMsg("对端关闭了连接: " + c.tag());//用close()优雅关闭
            return;
        }

        std::vector<std::string> got;//got来装接收到的信息条
        try {
            got = lines.feed(buf, static_cast<std::size_t>(n));
        } catch (const std::length_error& e) {
            logMsg("协议错误（" + c.tag() + "）: " + e.what());
            sendTo(c, "ERR|LINE_TOO_LONG|单行过长");
            return;
        }

        for (const std::string& line : got) {
            if (line.empty()) continue;
            ++line_no;  // 每条连接各自从 1 开始
            logMsg("[第 " + std::to_string(line_no) + " 行] " +
                   std::to_string(line.size()) + " 字节: " + line + "  (" + c.tag() + ")");
            if (!sendTo(c, "ECHO|" + std::to_string(line_no) + "|" + line)) {
                logMsg("回包失败，连接可能已断开（" + c.tag() + "）");
                return;
            }
        }

        if (lines.pending() > 0) {//pending()缓冲区里还剩下多少个字节的半行
            logMsg("（" + c.tag() + " 缓冲区里还留着半行 " +
                   std::to_string(lines.pending()) + " 字节，等后续数据）");
        }
    }
}

// 一个线程的生命周期：登记 -> 收发 -> 注销。shared_ptr 保证 Client 活到线程结束
void handleClient(std::shared_ptr<Client> c) { //线程入口函数
    logMsg("客户端已连接: " + c->tag() + "（当前连接数 " +
           std::to_string(g_active.load()) + "）");
    serveOne(*c);
    int left = --g_active; //client下线
    logMsg("连接结束: " + c->tag() + "（当前连接数 " + std::to_string(left) + "）");
}

}  // namespace

int main(int argc, char** argv) {
    const int port = (argc > 1) ? std::stoi(argv[1]) : 8888; //若启动时串了参数，就改成那个port

    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "socket: " << std::strerror(errno) << "\n";
        return 1;
    }
    int on = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    sockaddr_in addr{}; //IPv4地址结构体
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {//把这个 listen_fd 绑定到【IP + 端口】
        std::cerr << "bind: " << std::strerror(errno) << "（端口被占用？）\n";
        ::close(listen_fd);
        return 1;
    }
    if (::listen(listen_fd, 16) < 0) {
        std::cerr << "listen: " << std::strerror(errno) << "\n";
        ::close(listen_fd);
        return 1;
    }
    logMsg("服务端已监听 0.0.0.0:" + std::to_string(port) + "，等待连接...（Ctrl-C 结束）");

    // accept 循环：每来一个连接，建一个 Client、开一个线程，主线程立刻回到 accept
    while (true) {
        sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        int conn_fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (conn_fd < 0) {
            if (errno == EINTR) continue;  // 被信号打断，继续等
            logMsg(std::string("accept 失败: ") + std::strerror(errno));
            break;
        }

        auto c = std::make_shared<Client>();//建立新client结构体存放他的fd ip port，makeshared是应该拷贝
        c->fd = conn_fd;
        char ip[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        c->ip = ip;
        c->port = ntohs(peer.sin_port);

        ++g_active;
        // detach：连接的生命周期由线程自己管，主线程只管继续 accept
        std::thread t(handleClient, c);  // 新建线程，去跑handleClient函数
        t.detach();//主线程和子线程分离
    }

    ::close(listen_fd);
    return 0;
}
