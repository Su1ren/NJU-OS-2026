#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// Compile a function definition and load it
bool compile_and_load_function(const char *function_def) {
    // 找到函数名称
    // 函数必定返回 int，因此函数名称在 "int " 后面，直到第一个 '(' 为止
    // 跳过 int 之后的空格
    const char *start = function_def + 3; // 跳过 "int"
    while (*start == ' ') {
        start++;
    }
    const char *end = strchr(start, '(');
    if (!end) {
        perror("Invalid function definition: missing '('");
        return false; // 无效的函数定义
    }

    // 提取函数名称
    size_t name_len = end - start;
    // 生成临时文件名
    char src_path[] = "/tmp/crepl_XXXXXX.c";
    // 创建临时文件并写入函数定义
    int fd = mkstemps(src_path, 2); // 创建临时文件
    if (fd < 0) {
        perror("Failed to create temporary file");
        return false;
    }
    if (write(fd, function_def, strlen(function_def)) < 0 || write(fd, "\n", 1) < 0) {
        perror("Failed to write to temporary file");
        close(fd);
        unlink(src_path);
        return false;
    }
    close(fd);

    // 将临时文件编译为共享库
    printf("Function definition saved to %s\n", src_path);
    // 调用 exec 家族函数编译源代码为共享库
    char        so_path[] = "/tmp/crepl.so";
    char *const args[]    = {
        "gcc", "-x",    "c",      "-shared", "-fPIC", "-Wno-implicit-function-declaration",
        "-o",  so_path, src_path, NULL};
    // 创建临时文件收集 gcc 的错误输出，避免错误信息直接刷屏
    char err_path[] = "/tmp/crepl_err_XXXXXX";
    int  err_fd     = mkstemp(err_path);
    if (err_fd < 0) {
        perror("Failed to create error log file");
        unlink(src_path);
        return false;
    }

    // 在子进程中执行 gcc 命令
    pid_t pid = fork();
    if (pid < 0) {
        perror("Failed to fork");
        close(err_fd);
        unlink(err_path);
        unlink(src_path);
        return false;
    } else if (pid == 0) {
        // 子进程：把 stderr 接到错误日志文件，再执行 gcc
        if (err_fd != STDERR_FILENO) { // mkstemp 可能恰好返回 2
            dup2(err_fd, STDERR_FILENO);
            close(err_fd);
        }
        execvp("gcc", args);
        _exit(127); // exec 成功不会返回
    }

    // 父进程关闭自己的 fd 副本，之后按文件名重新打开读取
    close(err_fd);

    // 父进程等待子进程完成
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("Failed to wait for gcc");
        unlink(err_path);
        unlink(src_path);
        return false;
    }
    // 检查 WIFEXITED(status) 和 WEXITSTATUS(status) 来判断编译是否成功
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "Compilation failed\n");
        // 失败时才把 gcc 的错误输出展示出来
        FILE *err_file = fopen(err_path, "r");
        if (err_file) {
            char buf[512];
            while (fgets(buf, sizeof(buf), err_file)) {
                fputs(buf, stderr);
            }
            fclose(err_file);
        }
        unlink(err_path);
        unlink(src_path);
        return false;
    }
    unlink(err_path);
    unlink(src_path);
    printf("Compiled shared library: %s\n", so_path);
    return true;
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

    return false;
}

int main() {
    // 循环读取用户输入
    // 每次读取一行输入，判断是函数定义还是表达式
    // 判断标准是开头是否为 "int"
    // 1. 函数：将源代码保存到 /tmp/crepl_xxx.c 文件中，编译并加载函数
    // 2. 表达式：调用 evaluate_expression 函数计算结果并输出
    char  *line;
    size_t len = 0;
    while (true) {
        printf("$ ");
        if (getline(&line, &len, stdin) == -1) {
            break; // EOF
        }
        // 去掉换行符
        line[strcspn(line, "\n")] = 0;
        // 判断是函数定义还是表达式
        if (strncmp(line, "int", 3) == 0) {
            // 函数定义
            compile_and_load_function(line);
        } else {
            // 表达式
            int result;
            if (evaluate_expression(line, &result)) {
                printf("= %d\n", result);
            } else {
                printf("Error: Invalid expression\n");
            }
        }
    }
    return 0;
}
