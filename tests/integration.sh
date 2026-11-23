#!/usr/bin/env bash
# tests/integration.sh —— 第 3 块集成测试：登录
# 启动真正的 chat_server，用 bash 内置的 /dev/tcp 当客户端（不需要 netcat）。
#
# 覆盖：登录成功 / 重名 / 未登录被拒 / 昵称校验 / 缺字段 / 断线释放昵称 /
#       拆包 / 粘包 / QUIT / 超长行 / 多连接并发
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

disconnect3() { exec 3<&- 2>/dev/null; exec 3>&- 2>/dev/null; }
disconnect4() { exec 4<&- 2>/dev/null; exec 4>&- 2>/dev/null; }

read3() { reply=""; IFS= read -r -t 1 -u 3 reply || true; }
read4() { reply=""; IFS= read -r -t 1 -u 4 reply || true; }

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
    read3
    local r2=$reply
    disconnect3
    sleep 0.1
    stop_server
    if [ "$r1" = "OK|登录成功" ] && [ "$r2" = "ERR|BAD_FORMAT|暂不支持的消息类型: MSG" ]; then
        ok "L08 粘包：两行分别分派"
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

printf '\n通过 %d 项，失败 %d 项\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]