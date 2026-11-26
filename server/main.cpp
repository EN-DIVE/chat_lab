// server.cpp
#include "protocol.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// 多个连接线程都会写日志，不加锁会出现两行粘在一起
std::mutex g_log_mu;//globle log mutex全局日志锁

// HH:MM:SS，抓包式日志的时间戳
std::string nowStamp() {
    std::time_t t = std::time(nullptr); //获取当前系统时间（秒数）
    std::tm tm{};//tm结构体，用来存放拆开的时间时分秒......
    localtime_r(&t, &tm); // 把time的秒数转换为本地时区的时分秒，填入tm，线程安全版本，自己传自己创建的tm内存而非存入全局共享缓冲区
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);//格式化 时分秒
    return buf;
}

void logMsg(const std::string& msg) { //logMsg 的本质不是 “日志打印函数”，是**多线程安全的打印函数**，锁才是灵魂
    std::lock_guard<std::mutex> lk(g_log_mu);//lock_guard 自动锁，创建函数自动lock，离开大括号自动unlock--让它一旦开始日志函数就先lock锁住，等到打印完成之后出括号再自动解锁
    std::cout << "[" << nowStamp() << "] " << msg << "\n" << std::flush;//时间戳 + flush强行把缓冲区内容刷到终端，实时看到日志
}

// 判断是不是全空白 空格和tab
bool isBlank(const std::string& s) {
    return s.find_first_not_of(" \t") == std::string::npos;//找第一个不是空格、也不是 tab 的字符的位置
}

// fd 谁创建谁负责关闭：这里交给析构函数，线程结束时自动 close
struct Client { //新建client结构体
    int fd = -1; //socket，//fd 是这条 TCP 连接的句柄，内核早就记住了这条连接的四元组：(本机 IP, 本机端口，对端 IP, 对端端口)；fd 只是给你用户态程序用的一个 “快捷编号”，用来查内核里那张四元组表。
    std::string ip;
    int port = 0; //ip port 是client的地址
    std::string name;  // 用户昵称，空串 = 还没登录
    std::mutex wmu;  // 这条连接的写锁：保证整行不被别的线程插进来，本来自己只有自己的handleclient线程会sendto，但是有了广播，A上线了他的broadcast是在A的线程里面sendto其他人的，就造成了其他人可被多线程sendto

    ~Client() {
        if (fd >= 0) ::close(fd);
    }

    std::string tag() const { return ip + ":" + std::to_string(port); }//打印client地址的函数
    bool loggedIn() const { return !name.empty(); }  // 判断是否登录，已登录-empty=false-！取反-true，未登录-false
};

std::atomic<int> g_active{0};  // 当前TCP连接数(开着的线程数)，和后续登录计数无关！仅用于日志，统计当前在线客户端数量，g_active.load() // 读取当前值

// online表，昵称 -> 连接。value 用 shared_ptr，广播时连接对象不会被析构
std::mutex g_online_mu;//用户online表配套锁
std::map<std::string, std::shared_ptr<Client>> g_online; // map--key昵称 value这个昵称对应的client的指针

// 离线私聊暂存：昵称 -> 待投递的 PRIVIN 行，等这个人登录时一次性交给他
constexpr std::size_t kOfflineCap = 20; //每人最多存 20 条，超出丢最旧的
std::mutex g_offline_mu;//离线表配套锁
std::map<std::string, std::vector<std::string>> g_offline; //离线表，key-收件人昵称 value-待投递的完整协议行（vector）

// 所有发给这个客户端的数据统一走这个函数，强制上锁，统一规范。“广播时多个线程写同一连接”
bool sendTo(Client& c, const std::string& line) {
    //发出方向（s->c）的抓包日志。放在锁外打，日志锁和写锁不嵌套
    logMsg("[S->C] " + (c.name.empty() ? c.tag() : c.name) + " " +
           std::to_string(line.size()) + "B " + line); //[21:46:51] [C->S] alice 7B LIST
    std::lock_guard<std::mutex> lk(c.wmu);
    return chat::sendLine(c.fd, line);
}

// 广播函数：给所有已登录连接发一行，except传谁就跳过谁（用来排除发送者本人，自己上线不要给自己发消息）
void broadcast(const std::string& line, const std::shared_ptr<Client>& except = nullptr) {
    std::vector<std::shared_ptr<Client>> targets; //发送目标
    {
        std::lock_guard<std::mutex> lk(g_online_mu); //访问online就要加锁，锁内只拷client名单，绝对不能在这里 send！
        targets.reserve(g_online.size()); //reserve预留内存，一次性扩容
        for (const auto& kv : g_online) {
            if (kv.second != except) targets.push_back(kv.second); //second
        }
    }
    // sendTo 失败说明对方已经断了，不用管：它自己的 recv 线程迟早会发现并摘表
    for (const auto& t : targets) sendTo(*t, line);
}

// 离线私聊投递：登录成功后调用，把暂存的行一次性交给这个连接
void deliverOffline(const std::shared_ptr<Client>& c) {
    std::vector<std::string> pending;//待处理区
    {
        std::lock_guard<std::mutex> lk(g_offline_mu); //锁内只把队列搬空，不做 IO
        auto it = g_offline.find(c->name);
        if (it == g_offline.end()) return; //这个昵称没有暂存消息
        pending = std::move(it->second); //把整箱消息搬到局部变量
        g_offline.erase(it); //投递过就从表里删掉，避免下次重复发
    }
    for (const auto& line : pending) sendTo(*c, line); //锁外逐条发，和广播一个套路
    logMsg("投递离线消息 " + std::to_string(pending.size()) + " 条 -> " + c->name);
}

// 昵称规则 1~16 字节、不含控制字符（'|' 会被 split 切走，天然进不来）
bool validName(const std::string& name, std::string& why) { //昵称合法性校验 why--errMSG
    if (name.empty()) {
        why = "昵称不能为空";
        return false;
    }
    if (name.size() > 16) {
        why = "昵称最长 16 字节（一个汉字 3 字节）";
        return false;
    }
    if (name.find(',') != std::string::npos) { //广播ONLINE|a,b,c 用逗号分隔，昵称里带逗号列表就切乱了
        why = "昵称不能包含逗号（ONLINE 列表用逗号分隔）";
        return false;
    }
    if (name.find(' ') != std::string::npos) { //客户端 @昵称 用空格分隔，昵称带空格就永远 @ 不到
        why = "昵称不能包含空格（@昵称 私聊用空格分隔）";
        return false;
    }
    for (unsigned char ch : name) {
        if (ch < 0x20 || ch == 0x7f) { //<0x20是包括所有不可见的控制字符（换行等），0x7f del，不用考虑|，他会直接被split切走
            why = "昵称不能包含控制字符";
            return false;
        }
    }
    return true;
}

// LOGIN|昵称。成功登记 online 表并回 OK；失败回 ERR，连接保留（客户端可以改个名再来）
void handleLogin(const std::shared_ptr<Client>& c, const std::vector<std::string>& f) { //处理login业务，f是split切出的字段vector
    if (c->loggedIn()) { //判断连接是否登陆过，不能重复登陆
        sendTo(*c, "ERR|BAD_FORMAT|已经用 " + c->name + " 登录过了");
        return;
    }
    if (f.size() != 2) { //用户端输入的命令是不是被split切成两个字段
        sendTo(*c, "ERR|BAD_FORMAT|用法: LOGIN 昵称"); //说明文字里不能再出现 '|'
        return;
    }
    //以上其实client不会犯错，但是server不可以信任client！健壮性。

    const std::string& name = f[1];//f[1]是昵称名，后续所有昵称名都用name指代
    std::string why;
    if (!validName(name, why)) { //校验昵称合法性
        sendTo(*c, "ERR|BAD_FORMAT|" + why);
        return;
    }

    // 查重 + 登记必须在同一把锁里：分两步做的话，两个线程可能同时“查到没人用”，然后都登记成功
    bool taken = false;
    std::size_t online_count = 0; // 当前登录数
    {
        std::lock_guard<std::mutex> lk(g_online_mu); //这个大括号是锁的临界区，进括号加锁，出括号解锁
        taken = g_online.count(name) > 0; //查询 map 里面，key 等于 name 的条目有多少个
        if (!taken) {
            g_online[name] = c;
            c->name = name;
        }
        online_count = g_online.size(); //因为读g_online是要有锁的，所以在大括号里访问g_online的大小，拷贝到online count里
    }

    // 输出登录日志。锁外再发，持锁时不做IO，因为send recv可能会阻塞，占有锁。
    if (taken) {
        sendTo(*c, "ERR|NAME_TAKEN|昵称已被占用");
        return;
    }
    sendTo(*c, "OK|登录成功");
    logMsg("登录成功: " + name + " (" + c->tag() + ")，当前在线 " + //tag 打印client地址的函数
           std::to_string(online_count));

    // 离线消息必须在 OK 之后发：客户端登录阶段读到的第一行才是 OK，剩下的会留在 Reader 里给接收线程
    deliverOffline(c);

    // 告诉其他人。排除自己——自己刚收到 OK|登录成功 了
    broadcast("SYS|" + name + " 上线了", c);
}

// 处理list命令，查看在线用户列表，LIST -> ONLINE|a,b,c。map 的 key 本来就按字典序排好，直接遍历
void handleList(const std::shared_ptr<Client>& c) { //c请求者
    std::string names;
    {
        std::lock_guard<std::mutex> lk(g_online_mu); //锁内拷名单，name
        for (const auto& kv : g_online) {
            if (!names.empty()) names += ',';
            names += kv.first; //first
        }
    }
    sendTo(*c, "ONLINE|" + names); //锁外再发
}

// MSG|内容 -> CHAT|来源|内容，广播给其他人，发送者自己不发
void handleMsg(const std::shared_ptr<Client>& c, const std::vector<std::string>& f) { //群聊
    //先处理错误信息情况
    if (f.size() > 2) { //内容里出现'|'会被 split 切成多段，'|' 是协议分隔符，直接拒绝
        sendTo(*c, "ERR|BAD_FORMAT|内容里不能包含竖线");
        return;
    }
    if (f.size() != 2 || f[1].empty()) { //没有内容或内容为空，server 不信任 client：字段数、空内容都要自己挡
        sendTo(*c, "ERR|BAD_FORMAT|MSG 需要一个非空内容");
        return;
    }
    logMsg("群聊 " + c->name + " -> " + f[1]);
    // logmsg日志打印函数，来源用服务端记下的 c->name
    broadcast("CHAT|" + c->name + "|" + f[1], c);//除发送者广播出去
}

// PRIV|对象|内容 -> 只发给对象一个人，回包 PRIVIN|来源|内容
void handlePriv(const std::shared_ptr<Client>& c, const std::vector<std::string>& f) { //私聊
    if (f.size() > 3) { //内容里出现'|'会被 split 切成多段
        sendTo(*c, "ERR|BAD_FORMAT|内容里不能包含竖线");
        return;
    }
    if (f.size() != 3 || f[1].empty() || f[2].empty()) { //对象、内容一个都不能少
        sendTo(*c, "ERR|BAD_FORMAT|PRIV 需要对象和内容两个字段");
        return;
    }

    std::shared_ptr<Client> dst; //destination 目标连接，信息要发给谁
    {
        std::lock_guard<std::mutex> lk(g_online_mu); //查表要加锁
        auto it = g_online.find(f[1]); //在online表里查找f[1]，待发送对象的昵称
        if (it != g_online.end()) dst = it->second; //拷一份 shared_ptr 出来，出锁后对象不会被析构
        //shared_ptr保护的是Client 对象,使之若断开之后不被析构销毁，send还能运转只是返回-1
    }

    if (!dst) { //目标不在线：先暂存，等他登录再送
        {
            std::lock_guard<std::mutex> lk(g_offline_mu); //改离线表要加锁
            auto& box = g_offline[f[1]]; //key 是收件人昵称，没有就建一个新的。box拿到的是这个名称对应的value vector引用
            box.push_back("PRIVIN|" + c->name + "|" + f[2]); //存整行：原发送者 + 原内容
            if (box.size() > kOfflineCap) box.erase(box.begin()); //超过上限丢最旧的
        }
        logMsg("离线暂存 " + c->name + " -> " + f[1] + ": " + f[2]);
        sendTo(*c, "OK|对方不在线，消息已暂存，上线后送达"); //明确告诉发送者，不静默
        return;
    }
    logMsg("私聊 " + c->name + " -> " + f[1] + ": " + f[2]);
    // 点对点：只往目标一个fd去sendto
    if (!sendTo(*dst, "PRIVIN|" + c->name + "|" + f[2])) { //对方刚好在这期间断开
        sendTo(*c, "ERR|NO_SUCH_USER|用户不在线"); //不能让发送者以为消息送到了
    }
}

// 连接结束摘表，否则昵称永远被占，重连就是 NAME_TAKEN
void onDisconnect(const std::shared_ptr<Client>& c) { //下线清理函数
    if (!c->loggedIn()) return; //如果登陆状态时false，根本没登陆，不要操作online表

    {
        std::lock_guard<std::mutex> lk(g_online_mu); //改online表加锁
        auto it = g_online.find(c->name);
        // 只删自己那条：以后支持重登时，同名可能已经是另一个连接
        if (it != g_online.end() && it->second == c){ //1.g_online表里确实有name对应的client 2.这个name对应的client确实是我传的c，为了防止A的清理逻辑延迟执行、重复执行，在锁已经释放、昵称已经被别人抢走之后，再来执行删除，造成bug
            g_online.erase(it);
        }
    }
    logMsg("下线: " + c->name + " (" + c->tag() + ")");

    // c 已经从表里删掉，广播自然只发给剩下的人
    broadcast("SYS|" + c->name + " 下线了");
}

enum class Action { Keep, Close }; //连接保持keep，还是断开close

Action handleLine(const std::shared_ptr<Client>& c, const std::string& line) { //命令分发函数
    std::vector<std::string> f = chat::split(line, '|'); //先split切割命令行
    const std::string& cmd = f[0]; //取到第一个字段作为命令cmd

    if (cmd == "LOGIN") {
        handleLogin(c, f);
        return Action::Keep;
    }
    if (cmd == "QUIT") {
        if (f.size() != 1) { //QUIT不带参数，多出来的字段说明客户端不守协议
            sendTo(*c, "ERR|BAD_FORMAT|QUIT 不带参数");
            return Action::Keep;
        }
        sendTo(*c, "OK|再见");
        return Action::Close;
    }
    if (!c->loggedIn()) { //如果未登录，任何命令都不奏效
        sendTo(*c, "ERR|NEED_LOGIN|请先登录");
        return Action::Keep;
    }

    if (cmd == "LIST") { //LIST请求在线用户名单
        if (f.size() != 1) { //同样QUIT不接受多余字段
            sendTo(*c, "ERR|BAD_FORMAT|LIST 不带参数");
            return Action::Keep;
        }
        handleList(c);
        return Action::Keep;
    }

    if (cmd == "MSG") { //群聊
        handleMsg(c, f);
        return Action::Keep;
    }

    if (cmd == "PRIV") { //私聊
        handlePriv(c, f);
        return Action::Keep;
    }

    // 走到这里说明是协议里没定义的类型，回错即可
    std::string shown = cmd.substr(0, 16); //回显前截断，不会超长乱码原样弹回去
    sendTo(*c, "ERR|BAD_FORMAT|暂不支持的消息类型: " + shown);
    return Action::Keep;
}

// 单条连接的收发循环：第 1 块的逻辑原样搬进来，只是行号变成局部变量
void serveOne(const std::shared_ptr<Client>& c) {
    chat::LineBuffer lines;
    char buf[4096];

    while (true) {
        ssize_t n = ::recv(c->fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR) continue; //errno不是我定义的变量，是include包里面的内容返回错误原因，被信号打断，直接重试就好
            if (errno == ECONNRESET) {  // 对端强制重置连接如kill -9，这条socket废掉
                logMsg("连接被重置: " + c->tag());
            } else {
                logMsg("recv 失败（" + c->tag() + "）: " + std::strerror(errno));
            }
            return;
        }
        if (n == 0) {
            logMsg("对端关闭了连接: " + c->tag());//用close()优雅关闭
            return;
        }

        std::vector<std::string> got;//got来装接收到的信息条
        try {
            got = lines.feed(buf, static_cast<std::size_t>(n)); //feed按/n切割成命令条
        } catch (const std::length_error& e) {
            logMsg("协议错误（" + c->tag() + "）: " + e.what()); //logMSG server本地日志
            sendTo(*c, "ERR|BAD_FORMAT|单行过长"); //sendto 发给客户看
            return;
        }

        for (const std::string& line : got) { //遍历got里面的命令
            if (isBlank(line)) continue; //空行、纯空白行都直接丢掉
            // 接收方向（c->s）的抓包日志，已登录显示昵称，未登录显示 ip:port
            logMsg("[C->S] " + (c->loggedIn() ? c->name : c->tag()) + " " +
                   std::to_string(line.size()) + "B " + line);
            if (handleLine(c, line) == Action::Close) { //处理收到的命令，split
                logMsg("客户端主动退出: " + c->tag());
                return;
            }
        }

        if (lines.pending() > 0) {//pending()缓冲区里还剩下多少个字节的半行
            logMsg("（" + c->tag() + " 缓冲区里还留着半行 " +
                   std::to_string(lines.pending()) + " 字节，等后续数据）");
        }
    }
}

// 一个线程的生命周期：登记 -> 收发 -> 注销。shared_ptr 保证 Client 活到线程结束
void handleClient(std::shared_ptr<Client> c) { //线程入口函数
    logMsg("客户端已连接: " + c->tag() + "（当前连接数 " +
           std::to_string(g_active.load()) + "）");
    serveOne(c);
    onDisconnect(c);  // 退出前摘表
    int left = --g_active; //client下线
    logMsg("连接结束: " + c->tag() + "（当前连接数 " + std::to_string(left) + "）");
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);//广播时对方可能刚断，往已经断开的客户socket send，内核发这个SIGPIPE信号，默认杀死整个server进程；忽略它，坏连接只会 send 失败，不会带走整个服务端

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