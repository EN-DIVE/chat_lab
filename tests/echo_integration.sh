#!/usr/bin/env bash
# tests/echo_integration.sh —— 第 2 块集成测试（服务端已常驻，需主动 kill）
# 启动真正的 chat_server，用 bash 自带的 /dev/tcp 当客户端（不需要 netcat）。
#
# 用法：bash tests/echo_integration.sh ./build/chat_server [端口]
set -u

SERVER=${1:?用法: bash tests/echo_integration.sh <chat_server 路径> [端口]}
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

# 第 2 块起服务端常驻，用例结束后统一 kill；不再期望“自然退出”
stop_server() {
    if [ -n "$SPID" ]; then
        kill "$SPID" 2>/dev/null
        wait "$SPID" 2>/dev/null || true
        SPID=
    fi
}

server_alive() { [ -n "$SPID" ] && kill -0 "$SPID" 2>/dev/null; }

connect() {
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

disconnect()  { exec 3<&- 2>/dev/null; exec 3>&- 2>/dev/null; }
disconnect4() { exec 4<&- 2>/dev/null; exec 4>&- 2>/dev/null; }

read_reply() {
    reply=""
    read -r -t 1 -u 3 reply || true
}

expect_log() { grep -qF -- "$1" "$LOG"; }

# ---------- I01 单行往返 ----------
case_single_line() {
    start_server
    connect || { bad "I01 无法连接"; stop_server; return; }
    printf 'hello\n' >&3
    read_reply
    disconnect
    sleep 0.1
    stop_server
    if [ "$reply" = "ECHO|1|hello" ] && expect_log '[第 1 行] 5 字节: hello'; then
        ok "I01 单行：回包与日志正确"
    else
        bad "I01 单行：reply='$reply'"
    fi
}

# ---------- I02 一次写入多行（粘包） ----------
case_two_lines() {
    start_server
    connect || { bad "I02 无法连接"; stop_server; return; }
    printf 'first\nsecond\n' >&3
    read_reply; local r1=$reply
    read_reply; local r2=$reply
    disconnect
    sleep 0.1
    stop_server
    if [ "$r1" = "ECHO|1|first" ] && [ "$r2" = "ECHO|2|second" ] \
       && expect_log '[第 2 行] 6 字节: second'; then
        ok "I02 粘包：两行分别处理"
    else
        bad "I02 粘包：r1='$r1' r2='$r2'"
    fi
}

# ---------- I03 一行分两次写入（拆包） ----------
case_split_write() {
    start_server
    connect || { bad "I03 无法连接"; stop_server; return; }
    printf 'hel' >&3
    sleep 0.2
    printf 'lo\n' >&3
    read_reply
    disconnect
    sleep 0.1
    stop_server
    local count
    count=$(grep -c '^\[第 ' "$LOG")
    if [ "$reply" = "ECHO|1|hello" ] && expect_log '还留着半行 3 字节' \
       && [ "$count" = "1" ]; then
        ok "I03 拆包：半行被缓存，拼接后只处理一次"
    else
        bad "I03 拆包：reply='$reply' 行数=$count"
    fi
}

# ---------- I04 中文按字节计数 ----------
case_chinese() {
    start_server
    connect || { bad "I04 无法连接"; stop_server; return; }
    printf '你好\n' >&3
    read_reply
    disconnect
    sleep 0.1
    stop_server
    if [ "$reply" = "ECHO|1|你好" ] && expect_log '[第 1 行] 6 字节: 你好'; then
        ok "I04 中文：2 个汉字 = 6 字节"
    else
        bad "I04 中文：reply='$reply'"
    fi
}

# ---------- I05 空行被忽略，不占序号 ----------
case_empty_line() {
    start_server
    connect || { bad "I05 无法连接"; stop_server; return; }
    printf 'a\n\nb\n' >&3
    read_reply; local r1=$reply
    read_reply; local r2=$reply
    disconnect
    sleep 0.1
    stop_server
    if [ "$r1" = "ECHO|1|a" ] && [ "$r2" = "ECHO|2|b" ] \
       && ! grep -qF '[第 3 行]' "$LOG"; then
        ok "I05 空行：被忽略，序号仍为 1、2"
    else
        bad "I05 空行：r1='$r1' r2='$r2'"
    fi
}

# ---------- I06 超长行被拒绝 ----------
case_long_line() {
    start_server
    connect || { bad "I06 无法连接"; stop_server; return; }
    head -c 5000 /dev/zero | tr '\0' 'a' >&3
    sleep 0.3
    disconnect
    stop_server
    if expect_log '单行超过 kMaxLine'; then
        ok "I06 超长行：5000 字节无换行，服务端报协议错误"
    else
        bad "I06 超长行：日志里没有协议错误"
    fi
}

# ---------- I07 客户端断开后服务端仍存活 ----------
case_disconnect() {
    start_server
    connect || { bad "I07 无法连接"; stop_server; return; }
    disconnect
    sleep 0.2
    local still=0
    server_alive && still=1
    # 再连一次，证明 accept 循环没被前一个连接带走
    local again=0
    connect && again=1
    disconnect
    stop_server
    if [ "$still" = "1" ] && [ "$again" = "1" ] && expect_log '对端关闭了连接'; then
        ok "I07 断开：服务端记录关闭并继续接受新连接"
    else
        bad "I07 断开：still=$still again=$again"
    fi
}

# ---------- I08 两个连接并发，互不阻塞 ----------
case_concurrent() {
    start_server
    connect  || { bad "I08 A 连接失败"; stop_server; return; }
    connect4 || { bad "I08 B 连接失败"; disconnect; stop_server; return; }

    # A 什么都不发（阻塞在自己的 recv 里），B 先发
    printf 'from-B\n' >&4
    local rb=""
    read -r -t 1 -u 4 rb || true

    # A 依然可用，且自己的行号从 1 开始
    printf 'from-A\n' >&3
    local ra=""
    read -r -t 1 -u 3 ra || true

    disconnect4
    disconnect
    sleep 0.15
    stop_server

    local dup
    dup=$(grep -c '^\[第 1 行\]' "$LOG")
    if [ "$rb" = "ECHO|1|from-B" ] && [ "$ra" = "ECHO|1|from-A" ] \
       && [ "$dup" = "2" ]; then
        ok "I08 并发：两条连接各自独立，行号各从 1 开始"
    else
        bad "I08 并发：rb='$rb' ra='$ra' 第1行日志条数=$dup"
    fi
}

case_single_line
case_two_lines
case_split_write
case_chinese
case_empty_line
case_long_line
case_disconnect
case_concurrent

printf '\n通过 %d 项，失败 %d 项\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
