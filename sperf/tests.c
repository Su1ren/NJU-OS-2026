#define _GNU_SOURCE
#include "../testkit/testkit.h"

#include <string.h>

// TestKit lacks support of fork().
// 所以这里只测与 fork() 无关的纯逻辑：parse_strace_line / add_syscall /
// print_top_syscalls。启动 strace 的那条路径靠手工命令和 shell 脚本验证。

// ============================================================================
// 被测代码的声明
//
// tests.c 与 sperf.c 是两个独立编译的翻译单元，sperf.c 没有对外头文件，
// 因此这里必须重复写出类型定义与函数原型，且必须与 sperf.c 完全一致。
// ============================================================================

#define MAX_SYSCALLS 1024

typedef struct {
    char   name[64];
    double time;
} syscall_stat;

typedef struct {
    syscall_stat stats[MAX_SYSCALLS];
    int          count;
    double       total_time;
} syscall_stats;

int  parse_strace_line(char *line, char *syscall_name, double *time);
void add_syscall(syscall_stats *stats, const char *name, double time);
void print_top_syscalls(syscall_stats *stats, int n);

/**
 * 浮点近似比较。不用 fabs() 是为了不引入 libm（LDFLAGS 是空的）。
 */
static int near(double a, double b) {
    double d = a - b;
    return d < 1e-9 && d > -1e-9;
}

// ============================================================================
// 步骤 3：parse_strace_line
// ============================================================================

UnitTest(parse_normal_line) {
    // 典型的一行（注意用数组而不是字面量：函数会就地去掉行尾空白）
    char   line[] = "read(3, \"x\", 1) = 1 <0.000012>";
    char   name[64];
    double t = -1;

    tk_assert(parse_strace_line(line, name, &t) == 0, "should parse");
    tk_assert(strcmp(name, "read") == 0, "name = '%s', expect 'read'", name);
    tk_assert(near(t, 0.000012), "time = %.12f, expect 0.000012", t);
}

UnitTest(parse_pid_prefix) {
    // 多进程时 strace 会加 "[pid NNN] " 前缀，本实验虽然假设单进程，
    // 但解析仍需能跳过它
    char   line[] = "[pid 123] read(3, \"x\", 1) = 1 <0.0001>";
    char   name[64];
    double t = -1;

    tk_assert(parse_strace_line(line, name, &t) == 0, "should parse");
    tk_assert(strcmp(name, "read") == 0, "name = '%s', expect 'read'", name);
    tk_assert(near(t, 0.0001), "time = %.12f, expect 0.0001", t);
}

UnitTest(parse_rejects_exit_line) {
    char   line[] = "+++ exited with 0 +++";
    char   name[64];
    double t;

    tk_assert(parse_strace_line(line, name, &t) == -1, "non-syscall line must be rejected");
}

UnitTest(parse_rejects_signal_line) {
    char   line[] = "--- SIGCHLD {si_signo=SIGCHLD, si_code=CLD_EXITED, "
                    "si_pid=123, si_status=0} ---";
    char   name[64];
    double t;

    tk_assert(parse_strace_line(line, name, &t) == -1, "signal line must be rejected");
}

UnitTest(parse_forged_time_inside_args) {
    // 被追踪程序自己打印了一段 "<99999.9>"，它出现在行中间；
    // 真正的时间在行末，必须取行末那个
    char   line[] = "write(1, \"\\\", 1) = 100 <99999.9>\", 21)  = 21 <0.000149>";
    char   name[64];
    double t = -1;

    tk_assert(parse_strace_line(line, name, &t) == 0, "should parse");
    tk_assert(strcmp(name, "write") == 0, "name = '%s', expect 'write'", name);
    tk_assert(near(t, 0.000149), "time = %.6f; the forged 99999.9 leaked into statistics?", t);
}

UnitTest(parse_rejects_trailing_garbage) {
    // 行末不是 '>', 说明这不是一条完整的 trace 行（也可能是被 read() 截断的半行）
    char   line[] = "mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, 3, 0) = "
                    "0x7f0000000000 <0.000011>garbage";
    char   name[64];
    double t;

    tk_assert(parse_strace_line(line, name, &t) == -1, "line not ending with '>' must be rejected");
}

// ============================================================================
// 步骤 4：add_syscall / print_top_syscalls
// ============================================================================

UnitTest(add_accumulates_same_name) {
    syscall_stats st = {0};

    add_syscall(&st, "read", 3.0);
    add_syscall(&st, "read", 1.0); // 同名应累加到同一条记录
    add_syscall(&st, "write", 2.0);

    tk_assert(st.count == 2, "count = %d, expect 2 distinct syscalls", st.count);
    tk_assert(near(st.total_time, 6.0), "total_time = %f, expect 6.0", st.total_time);

    int i = 0;
    while (i < st.count && strcmp(st.stats[i].name, "read") != 0) {
        i++;
    }
    tk_assert(i < st.count, "read not recorded");
    tk_assert(near(st.stats[i].time, 4.0), "read = %f, expect 4.0", st.stats[i].time);
}

/**
 * 把 stdout 换成内存流执行 body（TestKit 内部也是这么做的）。
 * 注意：不能用 dup2(STDOUT_FILENO, ...) —— TestKit 已经把全局 stdout
 * 指向了自己的 fmemopen 流，此时直接写 fd 1 根本不会经过 stdout。
 */
static void capture_stdout(void (*body)(void *), void *arg, char **out, size_t *size) {
    FILE *mem = open_memstream(out, size);
    tk_assert(mem != NULL, "open_memstream() failed");

    FILE *saved = stdout;
    stdout      = mem;
    body(arg);
    stdout = saved;

    fclose(mem); // 之后 *out / *size 才有效
}

static void emit_two_syscalls(void *arg) {
    syscall_stats *st = arg;
    add_syscall(st, "read", 3.0);
    add_syscall(st, "write", 1.0);
    print_top_syscalls(st, 5); // total = 4.0 -> 75% / 25%
}

UnitTest(print_top_format_and_nuls) {
    syscall_stats st = {0};

    char  *out  = NULL;
    size_t size = 0;
    capture_stdout(emit_two_syscalls, &st, &out, &size);

    tk_assert(out != NULL, "nothing captured");

    // 1) 固定格式 {@code printf("%s (%d%%)\n", name, ratio)}
    tk_assert(strncmp(out, "read (75%)\n", 11) == 0,
              "first line should be 'read (75%%)', got '%.20s'", out);
    tk_assert(strstr(out, "write (25%)\n") != NULL, "missing 'write (25%%)'");

    // 2) 每轮输出后恰好 80 个 '\0'（数值 0，不是字符 '0'）
    size_t nuls = 0;
    for (size_t i = 0; i < size; i++) {
        if (out[i] == '\0') {
            nuls++;
        }
    }
    tk_assert(nuls == 80, "expect 80 NUL bytes, got %zu (total %zu bytes)", nuls, size);

    free(out);
}

static void emit_nothing(void *arg) {
    print_top_syscalls(arg, 5); // total_time == 0 时不应产生任何输出
}

UnitTest(print_top_silent_when_empty) {
    syscall_stats st = {0};

    char  *out  = NULL;
    size_t size = 0;
    capture_stdout(emit_nothing, &st, &out, &size);

    tk_assert(size == 0, "empty stats should print nothing, got %zu bytes", size);

    free(out);
}
