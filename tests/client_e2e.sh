#!/usr/bin/env bash
# tests/client_e2e.sh —— 用真客户端走完整链路
#   1. bob 先登录并挂住（stdin 用 sleep 撑着，不让它退出）
#   2. alice 依次：登录 -> 群聊 -> /list -> 私聊 -> /quit
#   3. 再用同名 bob 登录一次，验证 NAME_TAKEN 后能改名重登
#
# 用法：bash tests/client_e2e.sh <chat_server> <chat_client> [端口]
set -u

SERVER=${1:?用法: bash tests/client_e2e.sh <chat_server> <chat_client> [端口]}
CLIENT=${2:?用法: bash tests/client_e2e.sh <chat_server> <chat_client> [端口]}
PORT=${3:-18889}

TMP=$(mktemp -d)
SPID=
BPID=
PASS=0
FAIL=0

cleanup() {
    if [ -n "$BPID" ]; then kill "$BPID" 2>/dev/null; fi
    if [ -n "$SPID" ]; then
        kill "$SPID" 2>/dev/null
        wait "$SPID" 2>/dev/null
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT

ok()  { PASS=$((PASS + 1)); printf '[PASS] %s\n' "$1"; }
bad() { FAIL=$((FAIL + 1)); printf '[FAIL] %s\n' "$1"; }

expect() { grep -qF -- "$2" "$TMP/$1"; }

"$SERVER" "$PORT" > "$TMP/server.log" 2>&1 &
SPID=$!
sleep 0.3

# bob 登录后挂着不发消息；sleep 结束后管道 EOF，客户端自然退出
( sleep 6 ) | "$CLIENT" 127.0.0.1 "$PORT" bob > "$TMP/bob.out" 2>&1 &
BPID=$!
sleep 0.8

# alice 完整走一遍
{
    printf 'hello\n';   sleep 0.4
    printf '/list\n';   sleep 0.4
    printf '@bob hi\n'; sleep 0.5
    printf '/quit\n'
} | timeout 15 "$CLIENT" 127.0.0.1 "$PORT" alice > "$TMP/alice.out" 2>&1

# 重名登录：argv 传 bob（已被占），ERR 后从 stdin 拿 bob2 重登
{
    printf 'bob2\n'; sleep 0.5
    printf '/quit\n'
} | timeout 15 "$CLIENT" 127.0.0.1 "$PORT" bob > "$TMP/retry.out" 2>&1

wait "$BPID" 2>/dev/null || true
BPID=
sleep 0.2

if expect alice.out '登录成功' && expect alice.out '在线: alice, bob' \
   && expect alice.out '[私聊->bob] hi' && expect alice.out '已断开'; then
    ok "E01 alice：登录、/list、私聊、退出"
else
    bad "E01 alice 输出不完整：$(tr '\n' ' ' < "$TMP/alice.out" | head -c 300)"
fi

if expect bob.out 'alice: hello' && expect bob.out '[私聊] alice: hi' \
   && expect bob.out '[系统] alice 下线了'; then
    ok "E02 bob：收到群聊、私聊、下线通知"
else
    bad "E02 bob 输出不完整：$(tr '\n' ' ' < "$TMP/bob.out" | head -c 300)"
fi

if expect retry.out '登录失败' && expect retry.out 'NAME_TAKEN' \
   && expect retry.out '登录成功'; then
    ok "E03 重名：提示 NAME_TAKEN 后改名 bob2 登录成功"
else
    bad "E03 重名重登失败：$(tr '\n' ' ' < "$TMP/retry.out" | head -c 300)"
fi

if expect server.log '群聊 alice -> hello' && expect server.log '私聊 alice -> bob: hi'; then
    ok "E04 服务端：群聊/私聊都留下了日志"
else
    bad "E04 服务端日志缺失"
fi

printf '\n通过 %d 项，失败 %d 项\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]