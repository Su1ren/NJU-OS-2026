#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend.h"

static unsigned expr_counter = 0; // 用于生成唯一的 wrapper 函数名

// Compile a function definition and load it
bool compile_and_load_function(const char *function_def) {
    // 找到函数名称：函数必定返回 int，名称在 "int " 之后、第一个 '(' 之前
    const char *start = function_def + 3; // 跳过 "int"
    while (*start == ' ' || *start == '\t') {
        start++;
    }
    const char *end = strchr(start, '(');
    if (end == NULL || end == start) {
        fprintf(stderr, "Invalid function definition: missing name or '('\n");
        return false;
    }

    // 复制函数名，供后端查找入口符号（必须以 '\0' 结尾）
    size_t name_len = end - start;
    char   name[128];
    if (name_len >= sizeof(name)) {
        fprintf(stderr, "Function name too long\n");
        return false;
    }
    memcpy(name, start, name_len);
    name[name_len] = '\0';

    return crepl_backend_load(function_def, name) != NULL;
}

/**
 * @brief Evaluate an expression
 *
 * @param expression 传入的一行表达式
 * @param result 存储计算结果
 * @return true 表达式正常计算，结果存储在 result 中
 * @return false 求值失败，result 不可用
 */
bool evaluate_expression(const char *expression, int *result) {
    if (expression == NULL || result == NULL) {
        return false;
    }

    // 唯一的 wrapper 符号名
    char fn_name[64];
    int  name_len = snprintf(fn_name, sizeof(fn_name), "__expr_wrapper_%u", expr_counter++);
    if (name_len < 0 || (size_t)name_len >= sizeof(fn_name)) {
        return false;
    }

    // 构造 wrapper 函数的源代码：表达式成为无参函数体
    size_t src_size    = strlen(expression) + strlen(fn_name) + 32;
    char  *wrapper_src = malloc(src_size);
    if (wrapper_src == NULL) {
        return false;
    }
    snprintf(wrapper_src, src_size, "int %s(void) { return (%s); }", fn_name, expression);

    // 走同一套 编译 + 加载 流程，拿到函数地址
    int (*fn)(void) = (int (*)(void))crepl_backend_load(wrapper_src, fn_name);
    free(wrapper_src);
    if (fn == NULL) {
        return false;
    }

    // 在 crepl 主进程内直接调用
    *result = fn();
    return true;
}

// 判断一行是否只有空白字符
static bool is_blank(const char *line) {
    while (*line != '\0') {
        if (!isspace((unsigned char)*line)) {
            return false;
        }
        line++;
    }
    return true;
}

int main() {
    // 循环读取用户输入
    // 每次读取一行输入，判断是函数定义还是表达式
    // 判断标准是开头是否为 "int"
    // 1. 函数：将源代码保存到 /tmp/crepl_xxx.c 文件中，编译并加载函数
    // 2. 表达式：调用 evaluate_expression 函数计算结果并输出
    char  *line = NULL;
    size_t len  = 0;

    while (true) {
        printf(">> ");
        fflush(stdout); // 提示符没有换行，必须立即输出

        if (getline(&line, &len, stdin) == -1) {
            break; // EOF
        }

        // 去掉行尾的换行符（兼容 CRLF）
        line[strcspn(line, "\r\n")] = '\0';

        // 空行 / 纯空白行：跳过，不输出
        if (is_blank(line)) {
            continue;
        }

        // 判断是函数定义还是表达式
        if (strncmp(line, "int", 3) == 0) {
            // 函数定义：编译并加载进当前进程
            if (compile_and_load_function(line)) {
                printf("OK.\n");
            }
        } else {
            // 表达式：编译 wrapper 后在本进程内求值
            int result;
            if (evaluate_expression(line, &result)) {
                printf("= %d.\n", result);
            } else {
                printf("Error: Invalid expression\n");
            }
        }
        fflush(stdout); // 每轮结束立即输出
    }

    free(line);
    return 0;
}
