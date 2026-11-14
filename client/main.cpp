#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>

int main(int argc, char** argv) {  //读取命令行参数
    const std::string host = (argc > 1) ? argv[1] : "127.0.0.1"; //server IP，默认本机回环
    const int port = (argc > 2) ? std::stoi(argv[2]) : 8888; // server port
    std::string text = (argc > 3) ? argv[3] : "你好，我是客户端"; //传送给server的消息

    int fd = ::socket(AF_INET, SOCK_STREAM, 0); //创建client socket
    if (fd < 0) 
    { 
        std::cerr << "socket\n"; 
        return 1; 
    }

    sockaddr_in addr{}; //构建server的地址结构体
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "IP 不合法\n"; 
        ::close(fd); 
        return 1;
    }

    // connect(): 发起三次握手，本地端口由内核自动分配
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "connect: " << std::strerror(errno)
                  << "（服务端启动了吗？IP/端口对吗？）\n";
        ::close(fd);
        return 1;
    }

    //send（） 往TCP里传数据
    size_t sent = 0; //sent-已成功发出去的字节数
    while (sent < text.size()) {
        ssize_t n = ::send(fd, text.data() + sent, text.size() - sent, 0);//n>0，发送成功的字节；n<0，出错。
        if (n < 0) {
            if (errno == EINTR) continue;
            std::cerr << "send\n";
            break;
        }
        sent += static_cast<size_t>(n);
    }
    std::cout << "已发送 " << sent << " 字节: " << text << "\n";

    ::shutdown(fd, SHUT_WR);  // 关闭写方向，告诉服务端“我说完了”，对端 recv 会读到 0；读方向还没关！
    ::close(fd);
    return 0;
}