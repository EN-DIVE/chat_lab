#!/usr/bin/env bash
# tests/integration.sh —— 第 3~6 块集成测试：登录、在线列表、上下线通知、群聊、私聊
# 启动真正的 chat_server，用 bash 内置的 /dev/tcp 当客户端（不需要 netcat）。
#
# 覆盖：登录成功 / 重名 / 未登录被拒 / 昵称校验 / 缺字段 / 断线释放昵称 /
#       拆包 / 粘包 / QUIT / 超长行 / 多连接并发 / 上下线广播 / LIST / 逗号昵称 /
#       群聊广播 / 三人群聊 / 空消息 / 消息含竖线 / 私聊 / 私聊目标不在线
#
# 用法：bash tests/integration.sh ./build/chat_server [端口]
set -u

SERVER=${1:?用法: bash tests/integration.sh <chat_server 路径> [端口]}
PORT=${2:-18888}
LOG=$(mktemp)
SPID=
PASS=0
FAIL=0

cleanup() {
    exec 3<&- 2>/dev/null
    exec 3>&- 2>/dev/null
    exec 4<&- 2>/dev/null
    exec 4>&- 2>/dev/null
    exec 5<&- 2>/dev/null
    exec 5>&- 2>/dev/null
    if [ -n "$SPID" ]; then
        kill "$SPID" 2>/dev/null
        wait "$SPID" 2>/dev/null
    fi
    rm -f "$LOG"
}
trap cleanup EXIT

ok()  { PASS=$((PASS + 1)); printf '[PASS] %s\n' "$1"; }
bad() { FAIL=$((FAIL + 1)); printf '[FAIL] %s\n' "$1"; }

start_server() {
    : > "$LOG"
    "$SERVER" "$PORT" > "$LOG" 2>&1 &
    SPID=$!
}

stop_server() {
    if [ -n "$SPID" ]; then
        kill "$SPID" 2>/dev/null
        wait "$SPID" 2>/dev/null || true
        SPID=
    fi
}

server_alive() { [ -n "$SPID" ] && kill -0 "$SPID" 2>/dev/null; }

connect3() {
    for _ in $(seq 1 20); do
        if exec 3<>"/dev/tcp/127.0.0.1/$PORT" 2>/dev/null; then return 0; fi
        sleep 0.1
    done
    return 1
}

connect4() {
    for _ in $(seq 1 20); do
        if exec 4<>"/dev/tcp/127.0.0.1/$PORT" 2>/dev/null; then return 0; fi
        sleep 0.1
    done
    return 1
}

connect5() {
    for _ in $(seq 1 20); do
        if exec 5<>"/dev/tcp/127.0.0.1/$PORT" 2>/dev/null; then return 0; fi
        sleep 0.1
    done
    return 1
}

disconnect3() { exec 3<&- 2>/dev/null; exec 3>&- 2>/dev/null; }
disconnect4() { exec 4<&- 2>/dev/null; exec 4>&- 2>/dev/null; }
disconnect5() { exec 5<&- 2>/dev/null; exec 5>&- 2>/dev/null; }

read3() { reply=""; IFS= read -r -t 1 -u 3 reply || true; }
read4() { reply=""; IFS= read -r -t 1 -u 4 reply || true; }
read5() { reply=""; IFS= read -r -t 1 -u 5 reply || true; }

expect_log() { grep -qF -- "$1" "$LOG"; }

# ---------- L01 登录成功 ----------
case_login_ok() {
    start_server
    connect3 || { bad "L01 无法连接"; stop_server; return; }
    printf 'LOGIN|alice\n' >&3
    read3
    disconnect3
    sleep 0.1
    stop_server
    if [ "$reply" = "OK|登录成功" ] && expect_log '登录成功: alice' \
       && expect_log '当前在线 1'; then
        ok "L01 登录成功：回 OK，online 表登记"
    else
        bad "L01 登录：reply='$reply'"
    fi
}

# ---------- L02 重名被拒 ----------
case_dup_name() {
    start_server
    connect3 || { bad "L02 无法连接"; stop_server; return; }
    printf 'LOGIN|alice\n' >&3
    read3
    local r1=$reply
    connect4 || { bad "L02 第二条连接失败"; disconnect3; stop_server; return; }
    printf 'LOGIN|alice\n' >&4
    read4
    local r2=$reply
    disconnect4
    disconnect3
    sleep 0.1
    stop_server
    if [ "$r1" = "OK|登录成功" ] && [ "$r2" = "ERR|NAME_TAKEN|昵称已被占用" ]; then
        ok "L02 重名：第二个连接被 NAME_TAKEN 拒绝"
    else
        bad "L02 重名：r1='$r1' r2='$r2'"
    fi
}

# ---------- L03 未登录发 MSG 被拒 ----------
case_need_login() {
    start_server
    connect3 || { bad "L03 无法连接"; stop_server; return; }
    printf 'MSG|hi\n' >&3
    read3
    disconnect3
    sleep 0.1
    stop_server
    if [ "$reply" = "ERR|NEED_LOGIN|请先登录" ]; then
        ok "L03 未登录：MSG 被 NEED_LOGIN 挡住"
    else
        bad "L03 未登录：reply='$reply'"
    fi
}

# ---------- L04 昵称超过 16 字节 ----------
case_long_name() {
    start_server
    connect3 || { bad "L04 无法连接"; stop_server; return; }
    printf 'LOGIN|0123456789abcdefg\n' >&3
    read3
    disconnect3
    sleep 0.1
    stop_server
    if [ "$reply" = "ERR|BAD_FORMAT|昵称最长 16 字节（一个汉字 3 字节）" ]; then
        ok "L04 昵称校验：17 字节被 BAD_FORMAT 拒绝"
    else
        bad "L04 昵称校验：reply='$reply'"
    fi
}

# ---------- L05 LOGIN 缺昵称字段 ----------
case_missing_field() {
    start_server
    connect3 || { bad "L05 无法连接"; stop_server; return; }
    printf 'LOGIN\n' >&3
    read3
    disconnect3
    sleep 0.1
    stop_server
    if [ "$reply" = "ERR|BAD_FORMAT|用法: LOGIN|昵称" ]; then
        ok "L05 缺字段：回用法提示"
    else
        bad "L05 缺字段：reply='$reply'"
    fi
}

# ---------- L06 断开后昵称被释放 ----------
case_name_freed() {
    start_server
    connect3 || { bad "L06 无法连接"; stop_server; return; }
    printf 'LOGIN|alice\n' >&3
    read3
    local r1=$reply
    disconnect3
    sleep 0.3
    connect3 || { bad "L06 重连失败"; stop_server; return; }
    printf 'LOGIN|alice\n' >&3
    read3
    local r2=$reply
    disconnect3
    sleep 0.1
    stop_server
    if [ "$r1" = "OK|登录成功" ] && [ "$r2" = "OK|登录成功" ] \
       && expect_log '下线: alice'; then
        ok "L06 断线：昵称从 online 表摘掉，可再次登录"
    else
        bad "L06 断线：r1='$r1' r2='$r2'"
    fi
}

# ---------- L07 拆包：LOGIN 分两次写入 ----------
case_split_write() {
    start_server
    connect3 || { bad "L07 无法连接"; stop_server; return; }
    printf 'LOG' >&3
    sleep 0.2
    printf 'IN|alice\n' >&3
    read3
    disconnect3
    sleep 0.1
    stop_server
    if [ "$reply" = "OK|登录成功" ] && expect_log '还留着半行 3 字节'; then
        ok "L07 拆包：半行缓存后拼成 LOGIN|alice"
    else
        bad "L07 拆包：reply='$reply'"
    fi
}

# ---------- L08 粘包：登录与下一条消息在同一包里 ----------
case_pipeline() {
    start_server
    connect3 || { bad "L08 无法连接"; stop_server; return; }
    printf 'LOGIN|bob\nMSG|hi\n' >&3
    read3
    local r1=$reply
    read3                        # 第 5 块起 MSG 不给发送者回包，这里读超时为空
    local r2=$reply
    disconnect3
    sleep 0.1
    stop_server
    if [ "$r1" = "OK|登录成功" ] && [ -z "$r2" ] && expect_log '群聊 bob -> hi'; then
        ok "L08 粘包：两行分别分派，MSG 无回包但已广播"
    else
        bad "L08 粘包：r1='$r1' r2='$r2'"
    fi
}

# ---------- L09 QUIT 回包后服务端关闭连接 ----------
case_quit() {
    start_server
    connect3 || { bad "L09 无法连接"; stop_server; return; }
    printf 'LOGIN|amy\nQUIT\n' >&3
    read3
    local r1=$reply
    read3
    local r2=$reply
    local rc=0
    IFS= read -r -t 1 -u 3 _ || rc=$?
    disconnect3
    sleep 0.1
    stop_server
    if [ "$r1" = "OK|登录成功" ] && [ "$r2" = "OK|再见" ] && [ "$rc" = "1" ] \
       && expect_log '客户端主动退出'; then
        ok "L09 QUIT：回 OK|再见 后服务端关连接"
    else
        bad "L09 QUIT：r1='$r1' r2='$r2' eof_rc=$rc"
    fi
}

# ---------- L10 超长行被拒绝 ----------
case_long_line() {
    start_server
    connect3 || { bad "L10 无法连接"; stop_server; return; }
    head -c 5000 /dev/zero | tr '\0' 'a' >&3
    sleep 0.3
    read3
    disconnect3
    stop_server
    if [ "$reply" = "ERR|BAD_FORMAT|单行过长" ] && expect_log '单行超过 kMaxLine'; then
        ok "L10 超长行：5000 字节无换行，回 BAD_FORMAT 并断开"
    else
        bad "L10 超长行：reply='$reply'"
    fi
}

# ---------- L11 两个连接各自登录 ----------
case_concurrent() {
    start_server
    connect3 || { bad "L11 A 连接失败"; stop_server; return; }
    connect4 || { bad "L11 B 连接失败"; disconnect3; stop_server; return; }
    printf 'LOGIN|alice\n' >&3
    read3
    local r1=$reply
    printf 'LOGIN|bob\n' >&4
    read4
    local r2=$reply
    disconnect4
    disconnect3
    sleep 0.15
    stop_server
    if [ "$r1" = "OK|登录成功" ] && [ "$r2" = "OK|登录成功" ] \
       && expect_log '当前在线 2'; then
        ok "L11 并发：两条连接分别登录，online 表计数 2"
    else
        bad "L11 并发：r1='$r1' r2='$r2'"
    fi
}

# ---------- L12 上线广播 + LIST ----------
case_list_and_online() {
    start_server
    connect3 || { bad "L12 A 连接失败"; stop_server; return; }
    connect4 || { bad "L12 B 连接失败"; disconnect3; stop_server; return; }
    printf 'LOGIN|alice\n' >&3
    read3
    printf 'LOGIN|bob\n' >&4
    read4
    read3                        # alice 先收到 bob 的上线通知
    local sys=$reply
    printf 'LIST\n' >&3
    read3
    local list=$reply
    disconnect4
    disconnect3
    sleep 0.15
    stop_server
    if [ "$sys" = "SYS|bob 上线了" ] && [ "$list" = "ONLINE|alice,bob" ]; then
        ok "L12 上线广播 + LIST：SYS|bob 上线了，ONLINE|alice,bob"
    else
        bad "L12 广播/LIST：sys='$sys' list='$list'"
    fi
}

# ---------- L13 下线广播 ----------
case_offline_broadcast() {
    start_server
    connect3 || { bad "L13 A 连接失败"; stop_server; return; }
    connect4 || { bad "L13 B 连接失败"; disconnect3; stop_server; return; }
    printf 'LOGIN|alice\n' >&3
    read3
    printf 'LOGIN|bob\n' >&4
    read4
    read3                        # 先收掉 SYS|bob 上线了
    disconnect4                  # bob 直接断，不发 QUIT
    read3                        # 等下线通知
    local sys=$reply
    disconnect3
    sleep 0.15
    stop_server
    if [ "$sys" = "SYS|bob 下线了" ]; then
        ok "L13 下线广播：SYS|bob 下线了"
    else
        bad "L13 下线广播：sys='$sys'"
    fi
}

# ---------- L14 未登录 LIST 被拒 ----------
case_list_need_login() {
    start_server
    connect3 || { bad "L14 无法连接"; stop_server; return; }
    printf 'LIST\n' >&3
    read3
    disconnect3
    sleep 0.1
    stop_server
    if [ "$reply" = "ERR|NEED_LOGIN|请先登录" ]; then
        ok "L14 未登录：LIST 也被 NEED_LOGIN 挡住"
    else
        bad "L14 未登录 LIST：reply='$reply'"
    fi
}

# ---------- L15 昵称带逗号被拒 ----------
case_comma_name() {
    start_server
    connect3 || { bad "L15 无法连接"; stop_server; return; }
    printf 'LOGIN|a,b\n' >&3
    read3
    disconnect3
    sleep 0.1
    stop_server
    if [ "$reply" = "ERR|BAD_FORMAT|昵称不能包含逗号（ONLINE 列表用逗号分隔）" ]; then
        ok "L15 昵称校验：逗号会切乱 ONLINE 列表，被拒绝"
    else
        bad "L15 逗号昵称：reply='$reply'"
    fi
}

# ---------- L16 群聊：发送者自己收不到 ----------
case_group_chat() {
    start_server
    connect3 || { bad "L16 A 连接失败"; stop_server; return; }
    connect4 || { bad "L16 B 连接失败"; disconnect3; stop_server; return; }
    printf 'LOGIN|alice\n' >&3
    read3
    printf 'LOGIN|bob\n' >&4
    read4
    read3                        # 收掉 SYS|bob 上线了
    printf 'MSG|大家好\n' >&3
    read4
    local got=$reply
    read3                        # 发送者不该收到 CHAT，读超时为空
    local self=$reply
    disconnect4
    disconnect3
    sleep 0.15
    stop_server
    if [ "$got" = "CHAT|alice|大家好" ] && [ -z "$self" ] && expect_log '群聊 alice -> 大家好'; then
        ok "L16 群聊：bob 收到 CHAT|alice|大家好，alice 自己不收"
    else
        bad "L16 群聊：got='$got' self='$self'"
    fi
}

# ---------- L17 三人群聊：两个接收者都收到 ----------
case_three_way() {
    start_server
    connect3 || { bad "L17 A 连接失败"; stop_server; return; }
    connect4 || { bad "L17 B 连接失败"; disconnect3; stop_server; return; }
    connect5 || { bad "L17 C 连接失败"; disconnect4; disconnect3; stop_server; return; }
    printf 'LOGIN|alice\n' >&3
    read3
    printf 'LOGIN|bob\n' >&4
    read4
    read3                        # alice 收 SYS|bob 上线了
    printf 'LOGIN|carol\n' >&5
    read5
    read3                        # alice 收 SYS|carol 上线了
    read4                        # bob 收 SYS|carol 上线了
    printf 'MSG|hi\n' >&3
    read4
    local rb=$reply
    read5
    local rc=$reply
    read3                        # alice 自己不收
    local ra=$reply
    disconnect5
    disconnect4
    disconnect3
    sleep 0.15
    stop_server
    if [ "$rb" = "CHAT|alice|hi" ] && [ "$rc" = "CHAT|alice|hi" ] && [ -z "$ra" ]; then
        ok "L17 三人群聊：bob、carol 都收到，alice 自己不收"
    else
        bad "L17 三人群聊：rb='$rb' rc='$rc' ra='$ra'"
    fi
}

# ---------- L18 MSG 空内容被拒 ----------
case_empty_msg() {
    start_server
    connect3 || { bad "L18 无法连接"; stop_server; return; }
    printf 'LOGIN|alice\n' >&3
    read3
    printf 'MSG|\n' >&3
    read3
    disconnect3
    sleep 0.1
    stop_server
    if [ "$reply" = "ERR|BAD_FORMAT|MSG 需要一个非空内容" ]; then
        ok "L18 空消息：回 BAD_FORMAT"
    else
        bad "L18 空消息：reply='$reply'"
    fi
}

# ---------- L19 MSG 内容含竖线被拒 ----------
case_pipe_in_msg() {
    start_server
    connect3 || { bad "L19 无法连接"; stop_server; return; }
    printf 'LOGIN|alice\n' >&3
    read3
    printf 'MSG|a|b\n' >&3
    read3
    disconnect3
    sleep 0.1
    stop_server
    if [ "$reply" = "ERR|BAD_FORMAT|内容里不能包含竖线（第 8 块做转义）" ]; then
        ok "L19 消息含竖线：回 BAD_FORMAT"
    else
        bad "L19 消息含竖线：reply='$reply'"
    fi
}

# ---------- L20 私聊只送达对象，第三人看不到 ----------
case_private_chat() {
    start_server
    connect3 || { bad "L20 A 连接失败"; stop_server; return; }
    connect4 || { bad "L20 B 连接失败"; disconnect3; stop_server; return; }
    connect5 || { bad "L20 C 连接失败"; disconnect4; disconnect3; stop_server; return; }
    printf 'LOGIN|alice\n' >&3
    read3
    printf 'LOGIN|bob\n' >&4
    read4
    read3                        # alice 收 SYS|bob 上线了
    printf 'LOGIN|carol\n' >&5
    read5
    read3                        # alice 收 SYS|carol 上线了
    read4                        # bob 收 SYS|carol 上线了
    printf 'PRIV|bob|晚上自习吗\n' >&3
    read4
    local rb=$reply
    read5                        # carol 不该看到私聊
    local rc=$reply
    read3                        # 发送者也不收回包
    local ra=$reply
    disconnect5
    disconnect4
    disconnect3
    sleep 0.15
    stop_server
    if [ "$rb" = "PRIVIN|alice|晚上自习吗" ] && [ -z "$rc" ] && [ -z "$ra" ] \
       && expect_log '私聊 alice -> bob: 晚上自习吗'; then
        ok "L20 私聊：bob 收到 PRIVIN，carol 和 alice 都没收到"
    else
        bad "L20 私聊：rb='$rb' rc='$rc' ra='$ra'"
    fi
}

# ---------- L21 私聊目标不在线 ----------
case_private_offline() {
    start_server
    connect3 || { bad "L21 无法连接"; stop_server; return; }
    printf 'LOGIN|alice\n' >&3
    read3
    printf 'PRIV|nobody|hi\n' >&3
    read3
    disconnect3
    sleep 0.1
    stop_server
    if [ "$reply" = "ERR|NO_SUCH_USER|用户不在线" ]; then
        ok "L21 私聊目标不在线：回 NO_SUCH_USER"
    else
        bad "L21 目标不在线：reply='$reply'"
    fi
}

# ---------- L22 私聊缺内容 ----------
case_private_missing() {
    start_server
    connect3 || { bad "L22 无法连接"; stop_server; return; }
    printf 'LOGIN|alice\n' >&3
    read3
    printf 'PRIV|bob\n' >&3
    read3
    disconnect3
    sleep 0.1
    stop_server
    if [ "$reply" = "ERR|BAD_FORMAT|PRIV 需要对象和内容两个字段" ]; then
        ok "L22 私聊缺内容：回 BAD_FORMAT"
    else
        bad "L22 私聊缺内容：reply='$reply'"
    fi
}

# ---------- L23 私聊内容含竖线 ----------
case_private_pipe() {
    start_server
    connect3 || { bad "L23 无法连接"; stop_server; return; }
    printf 'LOGIN|alice\n' >&3
    read3
    printf 'PRIV|bob|a|b\n' >&3
    read3
    disconnect3
    sleep 0.1
    stop_server
    if [ "$reply" = "ERR|BAD_FORMAT|内容里不能包含竖线（第 8 块做转义）" ]; then
        ok "L23 私聊含竖线：回 BAD_FORMAT"
    else
        bad "L23 私聊含竖线：reply='$reply'"
    fi
}

case_login_ok
case_dup_name
case_need_login
case_long_name
case_missing_field
case_name_freed
case_split_write
case_pipeline
case_quit
case_long_line
case_concurrent
case_list_and_online
case_offline_broadcast
case_list_need_login
case_comma_name
case_group_chat
case_three_way
case_empty_msg
case_pipe_in_msg
case_private_chat
case_private_offline
case_private_missing
case_private_pipe

printf '\n通过 %d 项，失败 %d 项\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]