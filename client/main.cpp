// client.cpp 
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

//线程安全的打印，主要用于本地打印、回显等。这是敲完一行回车之后再打印的
//此时，回车符使得prompt_shown=false，本身就会开始打>，不需要再擦掉重打（这个是saypush）
void say(const std::string& s) { 
    std::lock_guard<std::mutex> lk(g_out_mu);
    std::cout << s << "\n" << std::flush;
}

std::atomic<bool> g_running{true}; //接收线程发现断线时置false，主线程据此退出

//提示符状态。屏幕上有没有 "> " 由它说了算，避免重复打印
std::atomic<bool> g_prompt_shown{false}; //全局提示词展示，原子变量无锁安全
// 标准输出是不是终端。用管道跑测试时不能发 ANSI 光标指令，否则输出里全是转义符
//只有确定输出是给人看的终端窗口，才去执行擦行、重绘提示符这种花哨操作；
//如果输出是给机器读、存日志、自动化测试，就关掉所有 ANSI 魔法控制码，只输出干净纯文本，不乱加东西。
bool g_tty = false; //true-终端

// 显示提示符（已有就不重复打）
void showPrompt() {
    std::lock_guard<std::mutex> lk(g_out_mu);
    if (!g_prompt_shown.load()) { //load 读取原子变量的值，如果是false 就打印>
        std::cout << "> " << std::flush;//flush强制输出
        g_prompt_shown = true;
    }
}

// 推送到达时先擦掉正在显示的 "> "和用户正在键入的消息，打印完再补回来
void sayPush(const std::string& s) { //server推送消息走saypush，本地提示用say
    std::lock_guard<std::mutex> lk(g_out_mu);
    if (g_tty && g_prompt_shown.load()) { //判断是终端，以及已经打印了
        std::cout << "\r\033[K"; // \r 回行首，\033[K 擦掉整行
        g_prompt_shown = false; //改为没打印<的状态
    }
    std::cout << s << "\n";
    if (g_tty && g_running.load()) { //检测是否在终端，并且与服务器正常连接
        std::cout << "> ";
        g_prompt_shown = true;
    }
    std::cout << std::flush; //为了把>从缓冲区刷到屏幕（cout默认缓冲区，可能打印了迟迟不显示）
}

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
    if (line.find("SYS|") == 0) { 
        return "[系统] " + line.substr(4);//从4位开始后面才是推送内容
    }
    if (line.find("ONLINE|") == 0) {
        std::string s = "在线: ";
        for (char ch : line.substr(7)) {
            s += ch;
            if (ch == ',') s += ' '; //ONLINE|a,b -> 在线: a, b
        }
        return s;
    }
    if (line.find("CHAT|") == 0) { //群聊：CHAT|来源|内容 -> 来源: 内容
        std::string rest = line.substr(5); //裁掉前缀CHAT|后剩下的部分
        std::size_t bar = rest.find('|');
        if (bar != std::string::npos) { //bar-|位置
            return rest.substr(0, bar) + ": " + rest.substr(bar + 1);
        }
    }
    if (line.find("PRIVIN|") == 0) { //私聊：PRIVIN|来源|内容 -> [私聊] 来源: 内容
        std::string rest = line.substr(7); //裁掉前缀PRIVIN|后剩下的部分
        std::size_t bar = rest.find('|');
        if (bar != std::string::npos) {
            return "[私聊] " + rest.substr(0, bar) + ": " + rest.substr(bar + 1);
        }
    }
    if (line.find("ERR|") == 0) { //ERR|代码|说明 -> [错误 代码] 说明
        std::size_t p = line.find('|', 4);
        if (p != std::string::npos) {
            return "[错误 " + line.substr(4, p - 4) + "] " + line.substr(p + 1);
        }
        return "[错误] " + line.substr(4);
    }
    return line;
}

}  // namespace

// 登录状态机：未登录 -> 已登录；连接断了转 Disconnected
enum class State { LoggedOut, LoggedIn, Disconnected };

int main(int argc, char** argv) {
    // 和server同理，服务端可能随时断，send 触发 SIGPIPE 会直接杀掉客户端，所以忽略SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);

    g_tty = ::isatty(STDOUT_FILENO) != 0; //判断是不是真终端，终端-g_tty true

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
    std::cout << "已连接 " << host << ":" << port
              << "，直接输入聊天，/help 查看命令\n";

    Reader reader(fd); //主线程创建了reader！！！

    //登录握手改成状态机，服务端回 ERR 就重新要昵称，不再直接退出
    State state = State::LoggedOut;
    std::string name = (argc > 3) ? argv[3] : ""; //如果一开始命令行直接传了 ./client ip port name，直接用
    std::string reply;

    //登录操作
    while (state == State::LoggedOut) { //如果登陆失败可以重复登录
        if (name.empty()) { //没传名字 提示输入
            std::cout << "昵称: " << std::flush;
            if (!std::getline(std::cin, name)) { //ctrl+D
                ::close(fd);
                return 1;
            }
            if (name.empty()) continue;
        }
        //本地合法性校验
        if (name.find('|') != std::string::npos) {//本地先按服务端规则挡一遍，少一趟往返
            std::cout << "昵称里不能含 '|'（会破坏一行一消息的协议）\n";
            name.clear();
            continue;
        }
        if (name.size() > 16) { 
            std::cout << "昵称最长 16 字节（一个汉字 3 字节）\n";
            name.clear();
            continue;
        }
        //发送LOGIN信息
        if (!chat::sendLine(fd, "LOGIN|" + name)) {
            std::cerr << "发送失败\n";
            ::close(fd);
            return 1;
        }
        //主线程用reader.next接收回包
        if (!reader.next(reply)) {
            std::cerr << "服务端已断开\n";
            ::close(fd);
            return 1;
        }
        if (reply.rfind("OK|", 0) == 0) {//登陆成功 break ； !=0即不是以OK|开头的
            state = State::LoggedIn;
            break;
        }
        // 登录失败：打印原因，把名字清空，下一圈重新提示
        std::cout << "登录失败: " << pretty(reply) << "\n";
        name.clear();
    }
    std::cout << "登录成功，输入一行回车发送，/help 查看命令\n";

    // 接收子线程。登录握手做完了，从现在开始服务端可能随时推消息
    std::thread recv_thread([&reader] {
        std::string line;
        while (reader.next(line)) {
            sayPush(pretty(line)); //推送和提示符共用一次加锁的输出，saypush函数先删>，再推送，后加上>
        }
        g_running = false;//reader.next返回false，循环退出，代表服务端中断，告诉主线程和server的连接断掉了
        sayPush("服务端已断开");
    });

    //下面开始解析用户输入命令
    std::string text;
    while (state == State::LoggedIn && g_running) { //已登录且服务端连接
        showPrompt(); //没有打印提示符就打印>

        if (!std::getline(std::cin, text)) break;  // Ctrl-D强制退出
        g_prompt_shown = false; //键入回车，就把prompt_shown改成false

        if (!g_running) break;  //等待输入期间服务端断了
        //下面开始处理text
        if (text.find_first_not_of(" \t") == std::string::npos) continue; //空白行直接忽略 空格 tab
        if (text == "/quit") {
            chat::sendLine(fd, "QUIT"); //回包由接收线程打印，主线程只负责上传
            state = State::Disconnected;
            break;
        }
        if (text == "/help") { 
            say("可用命令: /list 在线列表 | /quit 退出 | @昵称 内容 私聊 | 直接发送内容 群聊");
            continue;
        }
        if (text == "/list") { //获取在线用户列表
            if (!chat::sendLine(fd, "LIST")) break;
            continue;
        }

        if (text[0] == '@') { //@昵称 内容 -> 私聊
            std::size_t sp = text.find(' ');
            //先分成昵称 内容两段
            if (sp == std::string::npos || sp <= 1 || sp + 1 >= text.size()) {
                std::cout << "用法: @昵称 内容（如 @alice 要一起做chat_lab吗？）\n";
                continue;
            }
            std::string to = text.substr(1, sp - 1); //@后面到空格是对象
            std::string body = text.substr(sp + 1);  //空格后面是内容
            //进行合法性校验，不能有|
            if (to.find('|') != std::string::npos || body.find('|') != std::string::npos) {
                std::cout << "输入里不能含 '|'（协议用 | 分隔字段）\n";
                continue;
            }
            //长度校验
            std::string line = "PRIV|" + to + "|" + body; //先拼整行再量长度
            if (line.size() > chat::kMaxLine) { //在头文件里，单行上限4096
                say("这条太长了（" + std::to_string(line.size()) + " 字节，上限 " +
                    std::to_string(chat::kMaxLine) + "），没有发送");
                continue;
            }
            //发送私聊请求
            if (!chat::sendLine(fd, line)) {
                std::cerr << "发送失败\n";
                break;
            }
            say("[私聊->" + to + "] " + body); //服务端不回发送者，本地回显
            continue;
        }

        //群聊
        if (text.find('|') != std::string::npos) {
            std::cout << "输入里不能含 '|'（协议用 | 分隔字段）\n";
            continue;
        }

        std::string line = "MSG|" + text; //普通发言同样先拼整行再量长度
        if (line.size() > chat::kMaxLine) {
            say("这条太长了（" + std::to_string(line.size()) + " 字节，上限 " +
                std::to_string(chat::kMaxLine) + "），没有发送");
            continue;
        }
        // 普通群聊发言走 MSG
        if (!chat::sendLine(fd, line)) { //给他加上MSG前缀
            std::cerr << "发送失败\n";
            break;
        }
        say(name + ": " + text); //群聊：服务端不回发送者，自己的话本地打印一遍
    }

    state = State::Disconnected;
    //TCP全双工，读写是两条通道
    ::shutdown(fd, SHUT_WR);//shutdown优雅关闭写方向(SHUT_WR),告诉对方我不发了，server的recv->n=0
    if (recv_thread.joinable()) recv_thread.join(); //接收线程一直再跑reader.next()，等它把最后一行打完；joinable（）还在跑，join 主线程卡住，原地等子线程结束
    //等server关连接，reader.next返回false，接收线程退出循环
    ::close(fd);
    std::cout << "已断开\n";
    return 0;
}