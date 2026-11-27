// web_server.cpp —— 第二版第一步：手写 HTTP 服务端，只做静态文件
//
// 目标：浏览器访问 http://127.0.0.1:8080/ 能看到 web/index.html。
// 这一步只处理 GET，并且一个连接只收一个请求（响应后 Connection: close），
// 目的是先把“HTTP 报文边界”搞清楚：
//   请求 = 请求行 + 若干头 + 空行 + body
//   body 长度看 Content-Length（本步 GET 没有 body，第 2 步处理 POST 时再读）
//
// 运行：./web_server [端口] [静态目录]     默认 8080 web
// 约定：从仓库根目录启动，web 目录是相对路径
#include "protocol.h"  // 复用 chat::sendAll：send 可能只写出去一部分，必须循环

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace {

// 请求头上限，防止有人一直发头不发送`\r\n\r\n`结束头部，把内存撑爆
constexpr std::size_t kMaxHead = 16 * 1024;//`constexpr`编译期常量，请求头最大 16KB。

std::mutex g_log_mu;//全局日志锁

std::string nowStamp() { //HH:MM:SS时间戳
    std::time_t t = std::time(nullptr); //单位s
    std::tm tm{};
    localtime_r(&t, &tm);//本地时区时间填入tm结构体
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

void logMsg(const std::string& msg) { //日志打印函数 带锁
    std::lock_guard<std::mutex> lk(g_log_mu);
    std::cout << "[" << nowStamp() << "] " << msg << "\n" << std::flush;
}

// 保存http请求行
//HTTP请求报文最开头那一行，用来告诉服务器：我要做什么（方法）、要哪个资源（路径）、用哪个 HTTP 版本
struct Request {
    std::string method;   // GET / POST ...
    std::string path;     // 不含查询串，例如 /index.html
    std::string version;  // HTTP/1.1
};

// 读请求头：一直读到 "\r\n\r\n" 为止。
// HTTP 的 body 是二进制的，不能按字符串找结尾，本步 GET 没有 body，所以只读到头结束。
//GET client向server请求获得数据资源
bool readRequestHead(int fd, std::string& head) { //head传出参数，把头部取出到这个string里
    char buf[4096];
    while (head.find("\r\n\r\n") == std::string::npos) { //在已经收到的head里找结束标记，只要还没有找到头部结束标记，就继续循环收数据
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR) continue;//信号中断了正在进行的阻塞等待，不算真正错误，重新recv就好
            logMsg(std::string("recv 失败: ") + std::strerror(errno));
            return false;
        }
        if (n == 0) return false; // 对端在发完请求头前就关了
        head.append(buf, static_cast<std::size_t>(n));//刚收到的追加到head后面
        if (head.size() > kMaxHead) { // 一直没有空行，按坏请求处理
            logMsg("请求头超过 " + std::to_string(kMaxHead) + " 字节，拒绝");
            return false;
        }
    }
    return true;
}

// 解析请求行：GET /index.html HTTP/1.1
bool parseRequestLine(const std::string& head, Request& req) {
    std::size_t eol = head.find("\r\n");//\r\n换行
    std::string line = head.substr(0, eol); // 第一行就是请求行

    std::size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos) return false;
    std::size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return false;

    req.method = line.substr(0, sp1);//substr 起始位置 长度
    req.path = line.substr(sp1 + 1, sp2 - sp1 - 1); // 这里先不解析查询串
    req.version = line.substr(sp2 + 1);
    return true;
}

//转小写
std::string toLower(std::string s) {
    for (char& ch : s) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return s;
}

// 按扩展名给 Content-Type；浏览器靠它决定按 HTML 解析还是按脚本执行
// http响应头内的 content-type 就是MIME type。MIMEtype是HTTP传输时告诉对方数据类型的标签
std::string mimeType(const std::string& path) {
    std::size_t dot = path.rfind('.');
    std::string ext = (dot == std::string::npos) ? "" : toLower(path.substr(dot)); //转小写HTML和html都识别

    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js") return "application/javascript; charset=utf-8";
    if (ext == ".json") return "application/json; charset=utf-8";
    if (ext == ".txt") return "text/plain; charset=utf-8";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".ico") return "image/x-icon";
    return "application/octet-stream";
}

// 封装http响应报文并发送
// 统一拼响应：状态行 + 头 + 空行 + body
// Step 1，先统一 Connection: close：一条连接只回一个响应，边界最简单
void sendResponse(int fd, int status, const std::string& reason,
                  const std::string& contentType, const std::string& body,
                  const std::string& extraHeaders = "") { //fd 客户端socket status状态码 reason状态文字
    std::string resp;
    resp += "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n";
    resp += "Content-Type: " + contentType + "\r\n";
    resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    resp += "Cache-Control: no-store\r\n"; // 浏览器别缓存，后面轮询要拿新数据
    resp += "Connection: close\r\n";
    resp += extraHeaders;
    resp += "\r\n"; //空行
    resp += body;

    if (!chat::sendAll(fd, resp)) { // 和第一版一样：一次 send 可能写不完，调用sendall
        logMsg("发送响应失败，连接可能已断开");
    }
}

// 从磁盘里把静态文件读进内存，从而发到响应报文里；文件不存在返回 false
bool readFile(const std::string& path, std::string& out) { //path 磁盘文件路径 out 传出参数
    std::ifstream in(path, std::ios::binary); //二进制模式打开
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());//前 指向文件开头；后 代表文件末尾EOF。 out.assign(起点迭代器, 终点迭代器)`读到字符赋值给out：
    return true;
}

// 处理一个连接：读头 -> 解析 -> 路由 -> 回响应 -> 关闭（在调用方关闭）
void handleConnection(int fd, const std::string& webRoot) { //webroot 静态文件根目录，默认是/web
    std::string head;
    //接收请求报文头部
    if (!readRequestHead(fd, head)) return; // 还没读全，或对端已关
    //解析请求行
    Request req;
    if (!parseRequestLine(head, req)) {
        sendResponse(fd, 400, "Bad Request", "text/plain; charset=utf-8",
                     "400 请求行格式不对\n");
        return;
    }

    logMsg(req.method + " " + req.path + " " + req.version);
    //处理请求
    if (req.method != "GET") { // 第 1 步只支持 GET
        sendResponse(fd, 405, "Method Not Allowed", "text/plain; charset=utf-8",
                     "405 只支持 GET\n", "Allow: GET\r\n");
        return;
    }

    std::string path = req.path;

    // 查询串本步不处理，先截掉：/api/x?a=1 -> /api/x  截掉问号后面的查询参数（先不处理）
    std::size_t q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);

    if (path == "/") path = "/index.html"; // /是服务器资源根目录，HTTP服务器约定，访问目录时，自动在这个目录里找默认主页（index.html），这样访问域名即可自动跳转主页

    // 防目录穿越：路径里出现 ..（上一级目录） 直接拒绝，避免 GET /../../etc/passwd
    if (path.find("..") != std::string::npos || path.find('\\') != std::string::npos) {
        sendResponse(fd, 400, "Bad Request", "text/plain; charset=utf-8",
                     "400 路径不合法\n");
        return;
    }
    if (path.empty() || path[0] != '/') {
        sendResponse(fd, 400, "Bad Request", "text/plain; charset=utf-8",
                     "400 路径必须以 / 开头\n");
        return;
    }

    std::string body;
    std::string filePath = webRoot + path; // 例如 web + /index.html
    if (!readFile(filePath, body)) {
        logMsg("静态文件不存在: " + filePath);
        sendResponse(fd, 404, "Not Found", "text/plain; charset=utf-8",
                     "404 找不到 " + path + "\n");
        return;
    }

    sendResponse(fd, 200, "OK", mimeType(path), body);
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN); // 对端断开时 send 会触发 SIGPIPE，默认动作是杀进程

    const int port = (argc > 1) ? std::stoi(argv[1]) : 8080; //指定port
    const std::string webRoot = (argc > 2) ? argv[2] : "web"; // 指定静态文件夹，默认静态目录web

    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0); //监听 ipv4 TCP
    if (listen_fd < 0) {
        std::cerr << "socket: " << std::strerror(errno) << "\n";
        return 1;
    }
    int on = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)); //端口复用，服务端退出后，这个port可快速重启

    sockaddr_in addr{}; //监听地址结构体
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);//监听本机所有网卡（0.0.0.0），外部 / 本机都能连
    addr.sin_port = htons(port);
    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) { //bind！
        std::cerr << "bind: " << std::strerror(errno) << "（端口被占用？）\n";
        ::close(listen_fd);
        return 1;
    }
    if (::listen(listen_fd, 16) < 0) { //listen！监听态，16等待序列长度
        std::cerr << "listen: " << std::strerror(errno) << "\n";
        ::close(listen_fd);
        return 1;
    }
    logMsg("HTTP 服务已监听 0.0.0.0:" + std::to_string(port) +
           "，静态目录 " + webRoot + "（Ctrl-C 结束）");

    // 和第一版一样：每个连接一个线程，主线程只管继续 accept
    while (true) {
        sockaddr_in peer{}; //client地址ip port
        socklen_t peer_len = sizeof(peer);
        int conn_fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len); //建立连接
        if (conn_fd < 0) {
            if (errno == EINTR) continue;
            logMsg(std::string("accept 失败: ") + std::strerror(errno));
            break;
        }

        char ip[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        logMsg(std::string("新连接 ") + ip + ":" + std::to_string(ntohs(peer.sin_port)));//客户端二进制转换为字符串地址打印

        // 一个连接只处理一个请求，处理完由线程自己关 fd
        std::thread([conn_fd, webRoot] {
            handleConnection(conn_fd, webRoot);
            ::close(conn_fd);
        }).detach();
    }

    ::close(listen_fd);
    return 0;
}