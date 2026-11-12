"""模块二边界检查及菜单回归；只使用 Python 标准库和本机 Clang。

在项目根目录运行：python3 tests/check_module2.py
测试程序和可执行文件都放在临时目录，不更改正式数据。
"""
from pathlib import Path
import json
import math
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/xqyd.c"
passed = 0


def check(condition, name):
    global passed
    if not condition:
        raise AssertionError(name)
    passed += 1


def run(executable, args=(), text=""):
    result = subprocess.run(
        [str(executable), *args], input=text, text=True,
        capture_output=True, cwd=ROOT, timeout=10,
    )
    if result.returncode != 0 or result.stderr:
        raise AssertionError(result.stdout + result.stderr)
    return result.stdout


with tempfile.TemporaryDirectory(prefix="xqyd_module2_") as directory:
    temporary = Path(directory)
    harness_source = temporary / "input_check.c"
    harness_exe = temporary / "input_check"
    project_exe = temporary / "xqyd"

    # 在同一编译单元包含源码，以便直接检查 static 辅助函数。
    # 重命名原主函数，正式项目不需要为测试暴露额外接口。
    harness_source.write_text(
        '#define main project_main\n#include '
        + json.dumps(str(SOURCE), ensure_ascii=False)
        + '\n#undef main\n'
        + r'''
int main(int argc, char *argv[])
{
    int value = 123, ok;
    double amount = -1;
    char line[8];
    if (argc < 2) return 2;
    if (strcmp(argv[1], "spaces") == 0) {
        if (argc < 3) return 2;
        printf("RESULT %d\n", onlySpaces(argv[2]));
    } else if (strcmp(argv[1], "line") == 0) {
        ok = readLine("", line, sizeof(line));
        printf("RESULT %d [%s]\n", ok, ok ? line : "");
        ok = readLine("", line, sizeof(line));
        printf("NEXT %d [%s]\n", ok, ok ? line : "");
    } else if (strcmp(argv[1], "money") == 0) {
        ok = readThreshold(&amount);
        printf("RESULT %d %.17g\n", ok, amount);
    } else {
        int low = INT_MIN, high = INT_MAX;
        if (strcmp(argv[1], "month") == 0) { low = 1; high = 12; }
        if (strcmp(argv[1], "positive") == 0) low = 1;
        ok = readInt("", low, high, &value);
        printf("RESULT %d %d\n", ok, value);
    }
    return 0;
}
''', encoding="utf-8")

    # 未定义行为检查器可发现整数溢出；任何编译警告均视为失败。
    flags = ["clang", "-std=c11", "-Wall", "-Wextra", "-Wpedantic",
             "-Werror", "-fsanitize=undefined", "-fno-sanitize-recover=all"]
    for source, output in [(harness_source, harness_exe), (SOURCE, project_exe)]:
        subprocess.run([*flags, str(source), "-o", str(output)], check=True)

    def integer(mode, text, expected, rejected=False):
        out = run(harness_exe, [mode], text)
        check(out.rstrip().endswith(f"RESULT 1 {expected}"), repr(text))
        if rejected:
            check("之间的整数" in out, "非法整数应提示重输：" + repr(text))

    for text, expected in [("0", 0), ("-0", 0), ("+7", 7), ("007", 7),
                           (" \t-12 \t", -12), ("2147483647", 2147483647),
                           ("-2147483648", -2147483648)]:
        integer("integer", text + "\n", expected)
    for text in ["", "   ", "+", "-", "7abc", "1.5", "1 2", "--1",
                 "+ 1", "1e3", "2147483648", "-2147483649", "9" * 120]:
        integer("integer", text + "\n7\n", 7, rejected=True)
    for text in ["0", "13", "-1"]:
        integer("month", text + "\n12\n", 12, rejected=True)
    integer("positive", "H01\n1\n", 1, rejected=True)
    integer("integer", "9" * 200 + "\n8\n", 8, rejected=True)

    def money(text, expected, rejected=False):
        out = run(harness_exe, ["money"], text)
        tail = out.rsplit("RESULT ", 1)[1].split()
        check(tail[0] == "1" and math.isclose(float(tail[1]), expected,
              rel_tol=1e-15, abs_tol=1e-12), repr(text))
        if rejected:
            check("之间的金额" in out, "非法金额应提示重输：" + repr(text))

    for text, expected in [("1900", 1900), ("2025.50", 2025.5),
                           (" +20.5 \t", 20.5), (".5", .5), ("5.", 5),
                           ("0", 0), ("-0", 0), ("0.01", .01),
                           ("1000000000000.00", 1e12),
                           ("999999999999.99", 999999999999.99)]:
        money(text + "\n", expected)
    for text in ["", "   ", "+", "-", ".", "+.", "1.2.3", "NaN", "inf",
                 "1e3", "0x1p2", "-0.01", "1000000000001",
                 "1000000000000.01", "1000000000000.00000000000000001",
                 "20abc", "1 2", "+ 1", "9" * 120]:
        money(text + "\n2025.50\n", 2025.5, rejected=True)
    money("9" * 200 + "\n1900\n", 1900, rejected=True)

    for mode in ["integer", "money"]:
        for text in ["", "abc\n"]:
            out = run(harness_exe, [mode], text)
            check("RESULT 0 " in out, mode + "输入结束返回 0")
    for text, expected in [("", 1), (" \t\r\n", 1), (" a", 0), ("1", 0)]:
        out = run(harness_exe, ["spaces", text])
        check(out.strip() == f"RESULT {expected}", "尾部空白：" + repr(text))
    out = run(harness_exe, ["line"], "123456789012345\nOK\n")
    check("输入过长" in out and "RESULT 1 []" in out and "NEXT 1 [OK]" in out,
          "过长输入清除整行，不干扰下一次读取")
    out = run(harness_exe, ["line"], "abc\r\nOK\n")
    check("RESULT 1 [abc]" in out and "NEXT 1 [OK]" in out, "清除回车换行")

    # 通过真实菜单验证数据、计费、角色分流和排序等调用路径。
    scenarios = [
        ([0], ["1200 条记录，2 个小区，50 户"]),
        ([1, 1, 1, "H01", 2025, 0, 0], ["1570.00 kWh；全年总电费：837.00 元"]),
        ([1, 1, 2, "H01", 2025, 0, 0], ["1673.00 kWh；全年总电费：885.60 元"]),
        ([1, 1, 1, "NO01", 2025, 0, 0], ["未查询到该小区中的住户"]),
        ([2, 1, 7, 1900, 1, 0, 0], ["1 | 01 | 甲区 | 1950.00 | 40.00"]),
        ([2, 1, 7, 2025, 3, 0, 0], ["实际推荐 1 个", "1 | 02 | 乙区 | 2025.00 | 25.00"]),
        ([2, 1, 7, 2026, 3, 0, 0], ["没有符合条件的小区"]),
        ([2, 1, 8, 0, 2, 0, 0], ["1 | 01 | 甲区", "2 | 02 | 乙区", "| 30.00"]),
        ([2, 1, 9, 0, 2, 0, 0], ["1 | 02 | 乙区", "2 | 01 | 甲区", "| -5.00"]),
        ([2, 1, 7, "1e3", "2025.00", 3, 0, 0], ["之间的金额", "1 | 02 | 乙区"]),
    ]
    for mode in (1, 2, 3):
        scenarios.append(([1, 2, 2025, 7, mode, 0, 0], [
            "02 | 乙区 | 25 | 3125.00 | 3625.00 | 2025.00 | 145.00 | 81.00",
            "01 | 甲区 | 25 | 3000.00 | 3500.00 | 1950.00 | 140.00 | 78.00"]))
    for sequence, expected in scenarios:
        out = run(project_exe, text="\n".join(map(str, sequence)) + "\n")
        check(all(item in out for item in expected), "菜单回归：" + repr(sequence))

print(f"全部通过：{passed} 项断言（含输入边界、重输、EOF 和菜单回归）。")
