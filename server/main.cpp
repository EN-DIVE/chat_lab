#include <arpa/inet.h> //IP 地址转换（`inet_ntop`、`htonl/htons`）
#include <netinet/in.h>
#include <sys/socket.h> //socket 核心函数（socket、bind、listen、accept）
#include <unistd.h> //`close`关闭文件描述符（socket 本质就是 fd，和文件一样）
#include <cerrno>  //用来拿错误码，`strerror(errno)`把错误数字翻译成文字（比如 “端口被占用”）
#include <cstring>
#include <iostream>
#include <string>

int main(int argc, char** argv) { //你运行的时候可以写：./server 9000，就监听 9000 端口；
    const int port = (argc > 1) ? std::stoi(argv[1]) : 8888; //server port

    // 1. socket(): AF_INET（IPv4）+ SOCK_STREAM 即 TCP
    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);  //创建listenSocket
    if (listen_fd < 0) 
    { 
        std::cerr << "socket: " << std::strerror(errno) << "\n"; 
        return 1; 
    }

    int on = 1;  // 重启时端口可能处于TIME_WAIT，SO_REUSEADDR允许立即复用
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    // 2. bind(): 绑定“本机所有网卡 : port”
    sockaddr_in addr{};
    addr.sin_family = AF_INET;// IPv4地址
    addr.sin_addr.s_addr = htonl(INADDR_ANY); //填IP；`INADDR_ANY` 等价于 `0.0.0.0`，绑定所有网卡
    addr.sin_port = htons(port); //填端口；port=8888
    int bind_ret = ::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (bind_ret < 0)
    {
        std::cerr << "bind: " << std::strerror(errno) << "（端口被占用？）\n";
        ::close(listen_fd);
        return 1;
    }

    // 3. listen(): 端口进入监听，内核开始排队等待握手，socket改成listen模式
    if (::listen(listen_fd, 8) < 0) //8-维护队列长度，已经完成TCP三次握手，还未被accept取走
    { 
        std::cerr << "listen\n"; 
        ::close(listen_fd); 
        return 1;
    }
    std::cout << "服务端已监听 0.0.0.0:" << port << "，等待连接...\n" << std::flush;

    // 4. accept(): 阻塞等待；返回一个新的 fd 代表这条连接
    sockaddr_in peer{}; //peer里面装客户端IP和port
    socklen_t peer_len = sizeof(peer);
    int conn_fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len);// accept是读取队列中第一个client，把他的ip和port存进peer，并且给他绑定一个connectSocket
    if (conn_fd < 0) 
    { 
        std::cerr << "accept\n"; 
        ::close(listen_fd); 
        return 1; 
    }

    char ip[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));// 把二进制IP转成字符串
    std::cout << "客户端已连接: " << ip << ":" << ntohs(peer.sin_port) << "\n";// 网络端口转成本机字节序，打印

    // 5. recv(): 读取client发送来的数据，循环读到对端关闭（返回 0）
    char buf[4096]; //开辟缓冲区，暂时存放接收到的client文字
    while (true) {
        ssize_t n = ::recv(conn_fd, buf, sizeof(buf), 0);
        if (n > 0) {
            std::cout << "收到 " << n << " 字节: " << std::string(buf, n) << std::endl;
        } 
        else if (n == 0) {  //n=0，client挂断连接
            std::cout << "对端关闭了连接" << std::endl;
            break;
        } 
        else {
            if (errno == EINTR) continue;   // 被信号打断，重试
            std::cerr << "recv\n";
            break;
        }
    }

    ::close(conn_fd);
    ::close(listen_fd);
    return 0;
}