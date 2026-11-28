// web_server.cpp —— 第二版第二步：HTTP 路由与 JSON
//
// 第一步只把静态文件发出去；这一步补齐 HTTP 的另一半：
//   1. 按 Content-Length 把请求体读完整（body 是二进制，不能靠字符串找结尾）；
//   2. 增加 /api 路由：GET /api/ping、POST /api/echo；
//   3. 统一 JSON 响应与 JSON 错误格式；
//   4. 静态文件逻辑保持第一步的做法。
//
// 运行：./web_server [端口] [静态目录]     默认 8080 web
// 约定：从仓库根目录启动，web 目录是相对路径
#include "protocol.h"  // 复用 chat::sendAll

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
#include <nlohmann/json.hpp> //JSON 序列化/解析
#include <mutex>
#include <string>
#include <thread>

namespace {

constexpr std::size_t kMaxHead = 16 * 1024;// 请求头上限 16KB，防止有人一直发头不发送`\r\n\r\n`结束头部，把内存撑爆(k-const常量)
constexpr std::size_t kMaxBody = 64 * 1024; //请求体上限 64KB，超过回 413

std::mutex g_log_mu;//全局日志锁

std::string nowStamp() { //HH:MM:SS时间戳
    std::time_t t = std::time(nullptr); //单位s
    std::tm tm{};
    localtime_r(&t, &tm);//本地时区时间填入tm结构体
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);//格式化时分秒
    return buf;
}

void logMsg(const std::string& msg) { //日志打印函数 带锁
    std::lock_guard<std::mutex> lk(g_log_mu);
    std::cout << "[" << nowStamp() << "] " << msg << "\n" << std::flush;
}

//http请求报文，分成不同的部分储存
struct Request {
    //请求行
    std::string method;   // GET / POST ...
    std::string path;     // 不含查询串，例如 /index.html
    std::string version;  // HTTP/1.1
    //body
    std::string body;     //请求体（GET 为空，POST 按 Content-Length 读）
};

// 读请求头：一直读到 "\r\n\r\n" 为止。
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
    req.path = line.substr(sp1 + 1, sp2 - sp1 - 1);
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

// 读请求体：body 由 readRequestBody 按 Content-Length 单独读。
// 返回 0 成功；400 Content-Length 非法；411 POST 没有 Content-Length；413 body 超过上限；-1 连接出错。
int readRequestBody(int fd, const std::string& head, Request& req) {

    std::size_t end = head.find("\r\n\r\n"); //头部结束位置
    if (end == std::string::npos) return 400; // readRequestHead 已保证能找到

    std::size_t contentLength = 0;
    bool hasLength = false;
    std::size_t pos = head.find("\r\n"); // 先跳过请求行
    if (pos == std::string::npos) return 400;

    //在头部除请求行外，一行行扫content-length
    pos += 2;//跳过\r\n
    while (pos < end) {
        std::size_t eol = head.find("\r\n", pos);
        if (eol == std::string::npos || eol > end) {
            eol = end; //最后一行
        }
        std::string header = head.substr(pos, eol - pos);
        std::size_t colon = header.find(':');
        if (colon != std::string::npos) {
            std::string key = toLower(header.substr(0, colon));
            if (key == "content-length") {
                try {
                    contentLength = static_cast<std::size_t>(std::stoul(header.substr(colon + 1)));//string to unsigned long，字符串数字转成无符号整数
                    hasLength = true;
                } catch (...) {
                    return 400; // 非数字
                }
            }
        }
        pos = eol + 2;//下一行
    }

    if (req.method == "POST" && !hasLength) return 411; // POST 必须说明 body 多长
    if (contentLength > kMaxBody) return 413;

    // recv 读头时可能多读了 body 的前半截，这里要接着用
    req.body = head.substr(end + 4);
    if (req.body.size() > contentLength) req.body.resize(contentLength);//多余部分截断

    char buf[4096];
    while (req.body.size() < contentLength) { // 没读满就继续 recv，body 是二进制不能找结尾
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1; // body 没读完对端就关了
        req.body.append(buf, static_cast<std::size_t>(n));
    }
    if (req.body.size() > contentLength) req.body.resize(contentLength);
    return 0;
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

// 统一的 JSON 错误体，客户端只认 error.code / error.message
nlohmann::json errorBody(const std::string& code, const std::string& message) {
    return {{"error", {{"code", code}, {"message", message}}}}; //error是key，后面是key内的内容
}

// 发送JSON响应。dump() 会把中文按 UTF-8 原样输出并转义引号
void sendJson(int fd, int status, const std::string& reason,
              const nlohmann::json& body, const std::string& extraHeaders = "") {
    sendResponse(fd, status, reason, "application/json; charset=utf-8",
                 body.dump(), extraHeaders);//dump ：json 对象 → JSON 字符串
}

// 从磁盘里把静态文件读进内存，从而发到响应报文里；文件不存在返回 false
bool readFile(const std::string& path, std::string& out) { //path 磁盘文件路径 out 传出参数
    std::ifstream in(path, std::ios::binary); //二进制模式打开
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());//前 指向文件开头；后 代表文件末尾EOF。 out.assign(起点迭代器, 终点迭代器)`读到字符赋值给out：
    return true;
}

// /api 路由。路径已经去掉查询串，req.method 区分 GET/POST
void handleApi(int fd, const Request& req) {
    if (req.path == "/api/ping") { //探活请求：ping=看看服务器还活着没有。前端 JS 隔一会发一次 GET 请求，确认服务没有挂掉。
        if (req.method != "GET") {
            sendJson(fd, 405, "Method Not Allowed",
                     errorBody("METHOD_NOT_ALLOWED", "该接口只支持 GET"), "Allow: GET\r\n");
            return;
        }
        sendJson(fd, 200, "OK", {{"ok", true}});
        return;
    }

    if (req.path == "/api/echo") { //回显：验证 body 读取和 JSON 解析
        if (req.method != "POST") {
            sendJson(fd, 405, "Method Not Allowed",
                     errorBody("METHOD_NOT_ALLOWED", "该接口只支持 POST"), "Allow: POST\r\n");
            return;
        }
        nlohmann::json in;
        try {
            in = nlohmann::json::parse(req.body); //parse 反序列化/解析 字符串->json对象（类似map这种的键值对树）；body本身是string
        } catch (const std::exception&) {
            sendJson(fd, 400, "Bad Request", errorBody("BAD_JSON", "请求体不是合法 JSON"));
            return;
        }
        sendJson(fd, 200, "OK", {{"ok", true}, {"bytes", req.body.size()}, {"echo", in}});
        return;
    }

    sendJson(fd, 404, "Not Found", errorBody("NOT_FOUND", "接口不存在"));
}

// 处理一个连接：读头 -> 解析 -> 读 body -> 路由 -> 回响应 -> 关闭（在调用方关闭）
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

    //查询串先截掉，路由和静态文件都只用干净路径
    std::size_t q = req.path.find('?');
    if (q != std::string::npos) req.path = req.path.substr(0, q);
    
    const bool isApi = req.path.find("/api/") == 0; //是否api接头

    //POST 的 body 在这里读完；GET 没有 Content-Length 时就是空
    int bodyStatus = readRequestBody(fd, head, req); //返回0成功
    if (bodyStatus != 0) {
        if (isApi) { //接口请求统一回 JSON 错误，因为浏览器发了api过来就会用解析json的工具在那里等着，所以要发json回去
            if (bodyStatus == 411) {
                sendJson(fd, 411, "Length Required", errorBody("LENGTH_REQUIRED", "POST 需要 Content-Length"));
            } else if (bodyStatus == 413) {
                sendJson(fd, 413, "Payload Too Large", errorBody("BODY_TOO_LARGE", "请求体超过上限"));
            } else if (bodyStatus == 400) {
                sendJson(fd, 400, "Bad Request", errorBody("BAD_REQUEST", "Content-Length 不合法"));
            }
        } else { //静态资源请求保持纯文本错误
            if (bodyStatus == 411) {
                sendResponse(fd, 411, "Length Required", "text/plain; charset=utf-8",
                             "411 缺少 Content-Length\n");
            } else if (bodyStatus == 413) {
                sendResponse(fd, 413, "Payload Too Large", "text/plain; charset=utf-8",
                             "413 请求体过大\n");
            } else if (bodyStatus == 400) {
                sendResponse(fd, 400, "Bad Request", "text/plain; charset=utf-8",
                             "400 请求头不合法\n");
            }
        }
        return;
    }

    logMsg(req.method + " " + req.path + " " + req.version +
           " body=" + std::to_string(req.body.size()) + "B");

    ///api 走 JSON 路由，其余回落到静态文件
    if (isApi) {
        handleApi(fd, req);
        return;
    }

    //处理请求
    if (req.method != "GET") { // 静态资源只支持 GET
        sendResponse(fd, 405, "Method Not Allowed", "text/plain; charset=utf-8",
                     "405 静态资源只支持 GET\n", "Allow: GET\r\n");
        return;
    }

    std::string path = req.path;

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