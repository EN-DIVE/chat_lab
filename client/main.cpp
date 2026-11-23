// client.cpp —— 第 3 块：先登录，再收发
//
// 相对第 2 块改了两处：
//   1. 启动时拿昵称，发 LOGIN|昵称，收到 OK 才进入对话循环；
//   2. 回包改成用 Reader 读：一次收到多行也不丢（第 2 块只取第一行）。
// 仍是单线程“发一行 -> 等一行回包”，拆双线程要等服务端会主动推送（第 4/7 块）。
//
// 编译：g++ -std=c++17 -Wall -Wextra -pthread client.cpp -o client
// 运行：./client [IP] [端口] [昵称]
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
            pending_ = lines_.feed(buf, static_cast<std::size_t>(n));// 把读到的字节（在buf里）喂给LineBuffer，切分行
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

}  // namespace

int main(int argc, char** argv) {
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
    std::cout << "已连接 " << host << ":" << port << "，输入一行回车发送，/quit 退出\n";

    Reader reader(fd);

    // 新增：登录握手，OK 才继续
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

    std::string text;
    while (true) {
        std::cout << "> " << std::flush;//flush强制输出
        if (!std::getline(std::cin, text)) break;  // Ctrl-D强制退出
        if (text.empty()) continue;
        //如果输入quit
        if (text == "/quit") {
            chat::sendLine(fd, "QUIT");
            if (reader.next(reply)) std::cout << "服务端: " << reply << "\n";//reader就是接受并读取服务器响应
            break;
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
        if (!reader.next(reply)) { //不收到server回复就不能继续发
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