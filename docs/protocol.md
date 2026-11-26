# chat_lab 传输协议（v1_11.26）

以下是服务端和客户之间的通信协议。

## 1. 帧格式

- TCP 是字节流，没有天然的消息边界，因此约定一帧 = 一行 UTF-8 文本 + '\n'。
- 行内字段用 `|` 分隔，第一个字段是消息类型。
- 单行长度上限 4096 字节（不含换行），超过即视为协议错误。
- 字段内不允许出现 `|` 和换行。
- 接收端用 `LineBuffer` 缓存半行：一次 recv 收到半行或多行都能正确切分。

## 2. 具体协议总表

### （1）客户端 → 服务端

| 类型 | 格式 | 示例 | 说明 |
|---|---|---|---|
| LOGIN | `LOGIN\|昵称` | `LOGIN\|alice` | 昵称登录 |
| LIST | `LIST` | `LIST` | 请求在线列表，不带参数 |
| MSG | `MSG\|内容` | `MSG\|大家好` | 群聊 |
| PRIV | `PRIV\|对象\|内容` | `PRIV\|bob\|要一起做chat_lab吗？` | 私聊 |
| QUIT | `QUIT` | `QUIT` | 主动退出，不带参数 |

### （2）服务端 → 客户端

| 类型 | 格式 | 示例 | 说明 |
|---|---|---|---|
| OK | `OK\|说明` | `OK\|登录成功`、`OK\|对方不在线，消息已暂存，上线后送达` | 确认类回包，如登录成功、QUIT 再见等 |
| ERR | `ERR\|代码\|说明` | `ERR\|NAME_TAKEN\|昵称已被占用` | 出错回包 |
| ONLINE | `ONLINE\|a,b,c` | `ONLINE\|alice,bob` | 在线用户，按昵称字典序，逗号分隔 |
| CHAT | `CHAT\|来源\|内容` | `CHAT\|bob\|大家好` | 群聊推送，不发给发送者 |
| PRIVIN | `PRIVIN\|来源\|内容` | `PRIVIN\|bob\|要一起做chat_lab吗？` | 私聊推送，只发给目标一个连接，目标不在线时服务端暂存，上线后自动送达（每人最多 20 条） |
| SYS | `SYS\|说明` | `SYS\|bob 上线了` | 系统推送，上下线通知 |

## 3. 字段约束

| 字段 | 规则 |
|---|---|
| 昵称 | 1–16 字节（一个汉字 3 字节） |
| 昵称不含 `\|` | split 已天然挡住 |
| 昵称不含逗号 | `ONLINE\|a,b,c` 用逗号分隔，否则列表切乱 |
| 昵称不含空格 | 客户端用 `@昵称 内容` 发私聊，空格是分隔符 |
| 昵称不含控制字符 | 换行、制表符、DEL 等 |
| 消息内容 | 非空，不含 `\|` |
| 来源字段 | 一律由服务端从 online 表读取，不采用客户端自报的身份 |

## 4. 错误码

| 代码 | 含义 |
|---|---|
| NAME_TAKEN | 昵称已被占用 |
| NEED_LOGIN | 还没登录就发了需要登录才能执行的命令 |
| NO_SUCH_USER | 私聊发送时目标客户刚好断开 |
| BAD_FORMAT | 字段数量不对、内容为空、含 `\|`、单行过长、未知类型等 |

## 5. 一条消息的完整路径

基本设置：

| 执行体 | 职责 |
|---|---|
| 客户端主线程 | 登录握手、读键盘、按命令发请求、本地回显 |
| 客户端接收线程 | 循环 `Reader::next()`，把服务端推来的行交给 `pretty()` 打印 |
| 服务端主线程 | `accept` 循环，每来一个连接就创建一个 `Client` 和一个连接线程 |
| 服务端连接线程 | 每条连接一个客户，`serveOne` → `handleLine` → 视命令具体 `handleXxx` |

| 共享资源 | 锁 | 说明 |
|---|---|---|
| `g_online`（昵称 → `shared_ptr<Client>`） | `g_online_mu` | 在线表（map），登记在线客户 |
| 单条连接的发送 | `Client::wmu` | 保证同一连接上两条消息不会交错 |
| 服务端日志 | `g_log_mu` | 避免多线程日志粘行 |
| 客户端终端输出 | `g_out_mu` | 主线程和接收线程共用 |

基本流程：用户输入 → 客户端用户态缓冲 → 内核发送缓冲区 → TCP 字节流 → 服务端内核接收缓冲区 → `recv` → `LineBuffer` → 一行完整的协议报文。

### 5.1 建立连接

1. 服务端主线程 `socket / bind / listen`，卡在 `accept`。
2. 客户端主线程 `socket / connect`，三次握手完成。
3. 服务端 `accept` 返回 `conn_fd`，新建 `shared_ptr<Client>`结构体，填入 `fd / ip / port`，`++g_active`。
4. `std::thread(handleClient, c).detach()`：连接的生命周期交给这个线程，主线程回到 `accept`。
5. 连接线程进入 `serveOne`，创建自己的 `LineBuffer`，循环 `recv(c->fd, buf, 4096)`。

### 5.2 LOGIN 成功

1. 客户端主线程取昵称：命令行第 3 个参数，或提示输入；本地先检验 `|` 和是否超过 16 字节。
2. `sendLine(fd, "LOGIN|alice")`，通过调用 `sendAll` 循环写入内核发送缓冲区 → TCP 发出。
3. 此时接收线程还没启动，主线程自己用 `Reader::next()` 阻塞等待回包。
4. 服务端连接线程 `serveOne()` `recv` 到字节，`LineBuffer::feed` 凑齐一行；`isBlank` 判断非空白。
5. 打 `[C->S] <tag> 11B LOGIN|alice` 日志。
6. `handleLine` 用 `split(line, '|')` 得到 `{"LOGIN", "alice"}`，分派到 `handleLogin`。
7. `handleLogin` 依次判断：是否已登录 → 字段数是否为 2 → `validName` 是否通过。
8. 进入 `g_online_mu` 临界区：`count` 查重；昵称没人使用就 `g_online["alice"] = c` 并在 client 结构体写 `c->name`；记下在线人数；出锁。
9. 锁外 `sendTo(*c, "OK|登录成功")`：先打 `[S->C] alice ...` 日志，再调用了 `sendLine` 发往客户端。
10. 服务端日志记 `登录成功: alice (ip:port)，当前在线 N`。
11. `broadcast("SYS|alice 上线了", c)`：在 `g_online_mu` 内拷贝所有已登录连接（排除自己），出锁后逐个 `sendTo`。
12. 客户端主线程收到 `OK|登录成功`，状态从 `LoggedOut` 变为 `LoggedIn`。
13. 启动接收线程，主线程进入“读键盘 → 发请求”的循环。
14. 其他在线用户的接收线程收到 `SYS|alice 上线了`，`pretty()` 转成 `[系统] alice 上线了` 打印，终端上的 `> ` 提示符会被擦掉重画。


### 5.3 LOGIN 失败

可能原因：字段缺失、昵称为空、超过 16 字节、含 `|`、含逗号、含空格、含控制字符、昵称已被占用。

1. 客户端主线程发出 `LOGIN|...`，阻塞等回包。
2. 服务端走到 `handleLogin` 的某个校验分支，回 `ERR|代码|说明`（如 `ERR|NAME_TAKEN|昵称已被占用`），连接不关闭。
3. 失败分支不会写入 `g_online`，也不会广播 `SYS`。
4. 客户端主线程收到非 `OK` 回包：打印 `登录失败: [错误 NAME_TAKEN] 昵称已被占用`，清空昵称，重新提示输入。
5. 接收线程仍未启动，状态仍是 `LoggedOut`；用户改名后重走 5.2。
6. 只有两种情况会退出：`Reader::next()` 返回 false（服务端断了），或用户 Ctrl-D。

### 5.4 LIST 在线列表

1. 客户端主线程识别 `/list`，`sendLine(fd, "LIST")`。
2. 服务端 `split` 得到 `{"LIST"}`，调用 `handleList`；若 `f.size() != 1` 直接回 `ERR|BAD_FORMAT|LIST 不带参数`。
3. `handleList` 进入 `g_online_mu`，按 `std::map` 的 key 顺序（昵称字典序）加逗号拼成 `a,b,c`，出锁，列表包含请求者自己。
4. `sendTo(*c, "ONLINE|alice,bob")`。
5. 客户端接收线程收到后 `pretty()` 转成 `在线: alice, bob` 打印。


### 5.5 MSG 群聊

1. 合法性校验：客户端主线程，纯空白行及含 `|` 本地拒绝。将合法内容拼成 `MSG|内容`。
2. 检查整行长度是否超过 `kMaxLine`（4096），超了本地提示“没有发送”，连接不受影响。
3. `sendLine` 发出后，本地回显打印 `alice: 内容`（因为服务端不会回给发送者）。
4. 服务端：`recv → LineBuffer → [C->S] 日志 → handleLine → handleMsg`。
5. `handleMsg` 校验：字段数大于 2（说明内容含 `|`）→ `ERR|BAD_FORMAT|内容里不能包含竖线`；字段不足或内容为空 → `ERR|BAD_FORMAT|MSG 需要一个非空内容`。
6. 通过校验后记日志 `群聊 alice -> 内容`。
7. `broadcast("CHAT|alice|内容", c)`：
   - 锁 `g_online_mu`，把所有 `Client` 的 `shared_ptr` 拷进 `targets`，跳过发送者；随后释放锁；
   - 对每个 `targets[i]` 调用 `sendTo`：先打 `[S->C]` 日志，再拿该连接的 `wmu`，写 socket。
8. 其他每个客户端的接收线程收到 `CHAT|alice|内容`，`pretty()` 转成 `alice: 内容` 打印。
9. 单个接收者此时已断线的话，`sendTo` 返回 false，`broadcast` 不处理；该连接的 `recv` 线程会发现并摘表。

### 5.6 PRIV 成功私聊

1. 客户端主线程解析 `@bob 你好`：`@` 后到第一个空格是对象，空格后是内容；空格不对、含 `|`、超长都在本地拒绝。
2. 拼成 `PRIV|bob|你好` 通过 `sendLine` 发出，本地回显 `[私聊->bob] 你好`。
3. 服务端 `handlePriv` 校验：字段数大于 3 → `BAD_FORMAT`；对象或内容为空 → `BAD_FORMAT|PRIV 需要对象和内容两个字段`。
4. 进入 `g_online_mu`，`find("bob")`；找到就把目标的 `shared_ptr<Client>` 拷一份到 `dst`，出锁。
5. `dst` 为空 → `ERR|NO_SUCH_USER|用户不在线`。
6. 记日志 `私聊 alice -> bob: 你好`。
7. `sendTo(*dst, "PRIVIN|alice|你好")`：只写 `bob` 这一个 fd。
8. `bob` 的接收线程收到 `PRIVIN|alice|你好`，`pretty` 打印 `[私聊] alice: 你好`。

### 5.7 PRIV 目标不在线，或刚好断开

- **目标不在线**

1. `handlePriv` 在 `g_online_mu` 内查不到目标，`dst` 为空。
2. 进入 `g_offline_mu`，把 `PRIVIN|发送者|内容` 追加到 `g_offline[收件人]` 这个map里；超过 20 条时丢最旧的一条。
3. 出锁后回 `OK|对方不在线，消息已暂存，上线后送达`，发送者知道消息传达。
4. 目标下次登录：服务端回 `OK|登录成功` 之后调用 `deliverOffline`，锁内把整个队列搬空，锁外逐条 `sendTo`。
5. 客户端登录阶段读到的第一行仍是 `OK`，其余 `PRIVIN` 留在 `Reader::pending_`，接收线程启动后依次打印成 `[私聊] 发送者: 内容`。

**目标刚好断开**

1. 查表时还在，出锁后对方断开，`sendTo` 返回 false（`SIGPIPE` 已忽略）。
2. 回 `ERR|NO_SUCH_USER|用户不在线`。
3. 对方的 `recv` 线程随后读到 0 或 `ECONNRESET`，摘表并广播下线。

### 5.8 QUIT 主动退出

1. 客户端主线程识别 `/quit`，`sendLine(fd, "QUIT")`，状态置为 `Disconnected`，跳出循环。
2. 主线程 `shutdown(fd, SHUT_WR)`：只关写方向，告诉服务端“我不再发了”。
3. 主线程 `join` 接收线程（此时它可能还在 `recv`），等它把最后一行打完，然后 `close(fd)`，打印 `已断开`。
4. 服务端：若 `QUIT` 字段数不为 1 → 返回 `ERR|BAD_FORMAT|QUIT 不带参数`；否则回 `OK|再见`，返回 `Action::Close`。
5. `serveOne` 返回 → `handleClient` 调用 `onDisconnect`：在 `g_online_mu` 内 `find` 并校验 `it->second == c` 后 `erase`；记 `下线` 日志；`broadcast("SYS|alice 下线了")`（此时自己已不在表里，广播自然只发给剩下的人）。
6. 连接对象析构，`close(fd)`，对端收到 FIN。
7. 客户端接收线程先收到 `OK|再见` 打印，再 `recv` 返回 0，置 `g_running = false`，打印 `服务端已断开`；`join` 返回；主线程打印 `已断开`。
8. Ctrl-D 走的路径类似，只是不发 `QUIT`，服务端直接读到 EOF，日志记 `对端关闭了连接`，同样会摘表并广播下线。

### 5.9 SUMMARY 服务端推送的四类报文

| 报文 | 触发者 | 走哪条路 |
|---|---|---|
| `SYS` | 别人登录/下线 | `broadcast`（登录时排除自己，下线时自己已出表） |
| `CHAT` | 别人发 `MSG` | `broadcast` 排除发送者 |
| `PRIVIN` | 别人发 `PRIV` | 点对点，只发目标 fd |
| `ONLINE` | 自己发 `LIST` | 只回请求者 |

- 客户端对推送的处理统一是：接收线程 `Reader::next()` → `pretty()` 翻译 → `sayPush()` 加锁输出（tty 下先擦掉 `>` 再重画）。

## 6. 异常与处理措施

| 异常情况 | 发生位置 | 处理措施 |
|---|---|---|
| 半包 | 服务端/客户端 `LineBuffer` | 缓存半行，等后续 `recv` 拼齐，日志记录“还留着半行 N 字节” |
| 粘包 | 同上 | `feed` 沿 `\n` 切割，一次返回多行 |
| 昵称非法 / 重名 | 服务端/客户端 | 客户端先本地校验，服务端再次校验并检查重名。回 `ERR`，客户端打印原因、清空昵称、重新提示输入。 |
| 空白行 | 服务端/客户端 | 直接忽略空行，`continue` |
| 超长行 | 服务端/客户端 | 客户端本地提示不发送；不守协议的对端由服务端回`ERR\|BAD_FORMAT\|单行过长` 后断开（若已登录，`onDisconnect` 广播下线） |
| 格式错误 | 服务端具体 `handleXxx` | 回 `ERR\|BAD_FORMAT\|…` |
| 未知消息类型 | 服务端 `handleLine` 末尾 | 回 `ERR\|BAD_FORMAT\|暂不支持的消息类型: <前 16 字节>`，连接保留 |
| 用户突然关闭程序 | 服务端 `recv` | 返回 0 或 `ECONNRESET`，广播 `SYS` 下线并释放昵称 |
| 私聊对象离线 | 服务端 `handlePriv` | 暂存到 `g_offline[收件人]`（上限 20 条），向客户端回 `OK` 说明已暂存；对方登录后自动投递 |
| 私聊对象刚好断开 | 服务端 `handlePriv` | `send` 失败，同样回 `ERR\|NO_SUCH_USER` |
| 网络中断、服务端退出 | 客户端 `recv` | 返回 0，接收线程打印“服务端已断开”，主线程下次检查 `g_running == false` 时退出 |
| 主线程卡在输入 | 客户端 | 服务端断开时主线程可能还停在 `getline`，按回车或 Ctrl-D 才会退出 |
| 输入含 `\|` | 服务端/客户端 | 客户端本地拒绝，服务端 MSG/PRIV 也拒绝 |


## 7. 终端验证开始代码

```bash
# 终端 1
./build/chat_server 8888
# 终端 2
./build/chat_client 127.0.0.1 8888 alice
# 终端 3
./build/chat_client 127.0.0.1 8888 bob
```

## 8. v1 的限制

### 8.1 功能限制

- **身份只有昵称**：没有密码、没有注册流程，昵称释放后任何人都可以立刻使用。
- **只有一个全局聊天室**：所有登录用户都在同一个群里，没有好友、多人房间、群成员概念。
- **在线状态和离线消息都在内存里**：服务端重启后 `g_online`、`g_offline` 全部清空。
- **离线私聊按昵称暂存**：没有设置昵称注册表，给从未登录过的昵称发私聊也会被暂存。
- **离线暂存有上限**：每人最多 20 条，超出丢失最旧的一条。
- **没有历史消息**：群聊、私聊只在当下可见，命令行往上滚能看到的只是终端自己保留的屏幕内容。

### 8.2 协议与输入限制

- 单行上限 4096 字节（不含换行）；超长时客户端本地拦截，不守协议的对端由服务端回 `ERR|BAD_FORMAT|单行过长` 后断开。
- 内容不支持 `|`：它是字段分隔符。
- 昵称 1–16 字节，不含 `|`、逗号、空格、控制字符；前两条是为了 `split` 和 `ONLINE|a,b,c`，空格是为了 `@昵称 内容` 能被正确切分。


### 8.3 下一步计划

完成第二版网页聊天。