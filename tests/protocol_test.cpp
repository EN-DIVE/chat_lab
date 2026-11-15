// protocol_test.cpp —— 只测 LineBuffer / split，不需要网络
// 编译：g++ -std=c++17 -Wall -Wextra protocol_test.cpp -o protocol_test
// 运行：./protocol_test    （全部通过时打印 PASS，退出码 0）
#include "protocol.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failed = 0;

void check(bool ok, const std::string& name) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << "\n";
    if (!ok) ++g_failed;
}

std::vector<std::string> feed(chat::LineBuffer& lb, const std::string& s) {
    return lb.feed(s.data(), s.size());
}

void checkLines(chat::LineBuffer& lb,
                const std::string& input,
                const std::vector<std::string>& want,
                const std::string& name) {
    check(feed(lb, input) == want, name);
}

}  // namespace

int main() {
    // 1. 一条消息被切成三段喂进来（模拟 TCP 拆包）
    {
        chat::LineBuffer lb;
        check(feed(lb, "LOG").empty(), "拆包: 第一段还没凑成一行");
        check(feed(lb, "IN|al").empty(), "拆包: 第二段仍未结束");
        checkLines(lb, "ice\n", {"LOGIN|alice"}, "拆包: 第三段拼出完整一行");
        check(lb.pending() == 0, "拆包: 缓冲区已清空");
    }

    // 2. 一次喂进两行（模拟 TCP 粘包），第二行是 CRLF
    {
        chat::LineBuffer lb;
        checkLines(lb, "MSG|hi\nMSG|yo\r\n", {"MSG|hi", "MSG|yo"}, "粘包: 一次两行且去掉 CR");
    }

    // 3. 空行也是一行，交给上层决定要不要忽略
    {
        chat::LineBuffer lb;
        checkLines(lb, "A\n\nB\n", {"A", "", "B"}, "空行: 被原样切出来");
    }

    // 4. 汉字被切在半个字符上（UTF-8 多字节），拼回来必须完整
    {
        chat::LineBuffer lb;
        const std::string all = "MSG|你好\n";
        const std::string head = all.substr(0, 5);  // 'M''S''G''|' + “你”的第 1 个字节
        const std::string tail = all.substr(5);
        check(feed(lb, head).empty(), "中文: 半个汉字时不出行");
        check(feed(lb, tail) == std::vector<std::string>{"MSG|你好"}, "中文: 拼回后内容完整");
    }

    // 5. 半行长期挂起时，pending() 能反映出来（服务端收尾/调试用）
    {
        chat::LineBuffer lb;
        check(feed(lb, "PART").empty(), "半行: 未换行不出行");
        check(lb.pending() == 4, "半行: pending() 为 4");
    }

    // 6. 超长行要抛异常，避免内存被无限吃
    {
        chat::LineBuffer lb;
        bool thrown = false;
        try {
            feed(lb, std::string(chat::kMaxLine + 1, 'a'));
        } catch (const std::length_error&) {
            thrown = true;
        }
        check(thrown, "超长行: 抛出 length_error");
    }

    // 7. split 的三种常见形态
    {
        check(chat::split("PRIV|bob|hi", '|') == std::vector<std::string>{"PRIV", "bob", "hi"},
              "split: 三字段");
        check(chat::split("LOGIN", '|') == std::vector<std::string>{"LOGIN"}, "split: 单字段");
        check(chat::split("PRIV|", '|') == std::vector<std::string>{"PRIV", ""},
              "split: 尾随空字段");
    }

    std::cout << (g_failed == 0 ? "\n全部通过\n" : "\n有失败项\n");
    return g_failed == 0 ? 0 : 1;
}
