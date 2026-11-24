// client.cpp —— 第 5 块：群聊
//
// 第 4 块的收发线程不动，这一块只动展示：
//   1. pretty() 认识 CHAT|来源|内容；
//   2. 服务端不把群消息回给发送者，所以自己的话由客户端本地打印；
//   3. 其余 SYS / ONLINE 逻辑不变。
//
// 编译：g++ -std=c++17 -Wall -Wextra -pthread client.cpp -o client
// 运行：./client [IP] [端口] [昵称]
#include "protocol.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// 主线程和接收线程都会写终端，主线程-上行发数据，接收线程-接受server传来的数据
// 主线程也会打印 如：'>'给你提示输出
std::mutex g_out_mu;//全局输出锁，有c-s一问一答，也有s推送，两种输出不要竞态

void say(const std::string& s) { //线程安全的打印
    std::lock_guard<std::mutex> lk(g_out_mu);
    std::cout << s << "\n" << std::flush;
}

std::atomic<bool> g_running{true}; //接收线程发现断线时置false，主线程据此退出

// 按行读回包：缓冲区里已经切好的行先发出去，不够再 recv
class Reader { //封装reader类读回包
public:
    explicit Reader(int fd) : fd_(fd) {} //构造函数，fd是client自己的socket fd

    bool next(std::string& out) { //对外函数，out里面是拿到的信息，true成功拿到一行
        char buf[4096]; //缓冲区
        while (pending_.empty()) { //如果pending有，就直接用pending里切好的了
            ssize_t n = ::recv(fd_, buf, sizeof(buf), 0); // 缓冲区空，调用recv去内核拿新字节，存进buf
            if (n < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (n == 0) return false;  // 服务端关闭
            pending_ = lines_.feed(buf, static_cast<std::size_t>(n));// feed(data,n);把读到的字节（在buf里）喂给LineBuffer，切分行
        }
        out = std::move(pending_.front());//把字符所有权转移给out
        pending_.erase(pending_.begin());//把out刚取走的给删掉
        return true;
    }

private:
    int fd_;
    chat::LineBuffer lines_; //存的是初始数据，切完给pending之后，自己还剩下半行无/n的，下次再从buf拿数据就加在后面
    std::vector<std::string> pending_;  // pending（待处理），一次收到多行时，剩下的先存这儿，存已经切分好还没有被取走的完整行
};

// 把协议行翻译成给人看的一行
std::string pretty(const std::string& line) {
    if (line.rfind("SYS|", 0) == 0) { //rfind从右向左扫，又限制了在0号位置匹配前缀，所以变成了前缀检验工具，是==0，否==npos
        return "[系统] " + line.substr(4);//从4位开始后面才是推送内容
    }
    if (line.rfind("ONLINE|", 0) == 0) {
        std::string s = "在线: ";
        for (char ch : line.substr(7)) {
            s += ch;
            if (ch == ',') s += ' '; //ONLINE|a,b -> 在线: a, b
        }
        return s;
    }
    if (line.rfind("CHAT|", 0) == 0) { //第 5 块新增：CHAT|来源|内容 -> 来源: 内容
        std::string rest = line.substr(5); //裁掉前缀CHAT|后剩下的部分
        std::size_t bar = rest.find('|');
        if (bar != std::string::npos) { //bar-|位置
            return rest.substr(0, bar) + ": " + rest.substr(bar + 1);
        }
    }
    return line;
}

}  // namespace

int main(int argc, char** argv) {
    // 和server同理，服务端可能随时断，send 触发 SIGPIPE 会直接杀掉客户端，所以忽略SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);

    const std::string host = (argc > 1) ? argv[1] : "127.0.0.1"; //传了地址就用传的，没传就默认本机回环
    const int port = (argc > 2) ? std::stoi(argv[2]) : 8888; //同上，没传默认8888

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
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) { //发起连接
        std::cerr << "connect: " << std::strerror(errno) << "（服务端启动了吗？）\n";
        ::close(fd);
        return 1;
    }
    std::cout << "已连接 " << host << ":" << port << "，输入一行回车发送，/quit 退出，/list 查询在线用户\n";

    Reader reader(fd); //主线程创建了reader！！！

    // 登录握手，这里还是单线程，一问一答式，OK 才继续
    std::string name = (argc > 3) ? argv[3] : ""; //如果一开始命令行直接传了 ./client ip port name，直接用
    if (name.empty()) { //没传名字提示输入名字
        std::cout << "昵称: " << std::flush;
        if (!std::getline(std::cin, name)) { //ctrl+D
            ::close(fd);
            return 1;
        }
    }
    if (name.find('|') != std::string::npos) {
        std::cerr << "昵称里不能含 '|'（会破坏一行一消息的协议）\n";
        ::close(fd);
        return 1;
    }

    if (!chat::sendLine(fd, "LOGIN|" + name)) {
        std::cerr << "发送失败\n";
        ::close(fd);
        return 1;
    }
    std::string reply;
    if (!reader.next(reply)) {
        std::cerr << "服务端已断开\n";
        ::close(fd);
        return 1;
    }
    if (reply.rfind("OK|", 0) != 0) {//!=0即不是以OK|开头的
        std::cerr << "登录失败: " << reply << "\n";
        ::close(fd);
        return 1;
    }
    std::cout << "登录成功，输入一行回车发送，/quit 退出\n";

    // 新建接收子线程。登录握手做完了，从现在开始服务端可能随时推消息
    std::thread recv_thread([&reader] {
        std::string line;
        while (reader.next(line)) {
            say(pretty(line)); //线程不断调用reader.next，say-带锁的print
        }
        g_running = false;//reader.next返回false，循环退出，代表服务端中断，告诉主线程和server的连接断掉了
        say("服务端已断开");
    });

    std::string text;
    while (g_running) {
        { //提示符也要和接收线程抢终端，包在输出锁里
            std::lock_guard<std::mutex> lk(g_out_mu);
            std::cout << "> " << std::flush;//flush强制输出
        }
        if (!std::getline(std::cin, text)) break;  // Ctrl-D强制退出
        if (text.empty()) continue;
        //如果输入quit
        if (text == "/quit") {
            chat::sendLine(fd, "QUIT"); //回包由接收线程打印，主线程只负责上传
            break;
        }
        if (text == "/list") { //获取在线用户列表
            if (!chat::sendLine(fd, "LIST")) break;
            continue;
        }
        if (text.find('|') != std::string::npos) {
            std::cout << "输入里不能含 '|'（第 8 块做转义）\n";
            continue;
        }

        // 如果是信息，第 3 块起服务端按类型分派，普通发言走 MSG
        if (!chat::sendLine(fd, "MSG|" + text)) { //给他加上MSG前缀
            std::cerr << "发送失败\n";
            break;
        }
        say(name + ": " + text); //群聊：服务端不回发送者，自己的话本地打印一遍
    }

    ::shutdown(fd, SHUT_WR);//shutdown优雅关闭写方向
    if (recv_thread.joinable()) recv_thread.join(); //等接收线程把最后一行打完
    ::close(fd);
    std::cout << "已断开\n";
    return 0;
}