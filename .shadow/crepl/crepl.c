#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned expr_counter = 0; // 用于生成唯一的 wrapper 函数名

#define MAX_LOADED 512 // 保留已加载共享库的 handle，避免被卸载

static void  *loaded_handles[MAX_LOADED];
static size_t n_loaded = 0;

// 写满 len 字节，处理部分写与 EINTR
static bool write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        buf += n;
        len -= (size_t)n;
    }
    return true;
}

// 在子进程中调用 gcc 编译 c_path -> so_path，错误输出收集到临时文件
static bool run_gcc(const char *c_path, const char *so_path) {
    char *const args[] = {"gcc",   "-x",   "c",    "-shared",
                          "-fPIC", "-Wno-implicit-function-declaration",
                          "-o",    (char *)so_path, (char *)c_path, NULL};

    char err_path[] = "/tmp/crepl_err_XXXXXX";
    int  err_fd     = mkstemp(err_path);
    if (err_fd < 0) {
        perror("Failed to create error log file");
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("Failed to fork");
        close(err_fd);
        unlink(err_path);
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

    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("Failed to wait for gcc");
        unlink(err_path);
        return false;
    }
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
        return false;
    }
    unlink(err_path);
    return true;
}

// 公共流程：写临时 .c -> 编译唯一路径的 .so -> dlopen -> dlsym
// 成功返回符号地址（handle 保持加载），失败返回 NULL
static void *compile_source(const char *source, const char *symbol) {
    char src_path[] = "/tmp/crepl_XXXXXX.c";
    int  fd         = mkstemps(src_path, 2);
    if (fd < 0) {
        perror("Failed to create temporary file");
        return NULL;
    }
    if (!write_all(fd, source, strlen(source)) || !write_all(fd, "\n", 1)) {
        perror("Failed to write to temporary file");
        close(fd);
        unlink(src_path);
        return NULL;
    }
    close(fd);

    // 唯一的 so 路径：从 .c 路径派生，避免 dlopen 命中旧缓存
    char so_path[sizeof(src_path) + 4];
    snprintf(so_path, sizeof(so_path), "%s.so", src_path);

    if (!run_gcc(src_path, so_path)) {
        unlink(src_path);
        return NULL;
    }
    unlink(src_path); // 编译完成，源码文件可以删除

    void *handle = dlopen(so_path, RTLD_NOW | RTLD_GLOBAL);
    if (handle == NULL) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        unlink(so_path);
        return NULL;
    }
    unlink(so_path); // 映射已建立，文件可以删除

    dlerror(); // 清除已有错误状态
    void       *sym = dlsym(handle, symbol);
    const char *err = dlerror();
    if (err != NULL || sym == NULL) {
        fprintf(stderr, "dlsym: %s\n", err ? err : "symbol not found");
        dlclose(handle);
        return NULL;
    }

    // 保留 handle：不 dlclose，保证全局符号一直可用于后续库的解析
    if (n_loaded < MAX_LOADED) {
        loaded_handles[n_loaded++] = handle;
    }
    return sym;
}

// Compile a function definition and load it
bool compile_and_load_function(const char *function_def) {
    // 找到函数名称：函数必定返回 int，名称在 "int " 之后、第一个 '(' 之前
    const char *start = function_def + 3; // 跳过 "int"
    while (*start == ' ') {
        start++;
    }
    const char *end = strchr(start, '(');
    if (end == NULL || end == start) {
        fprintf(stderr, "Invalid function definition: missing name or '('\n");
        return false;
    }

    // 复制函数名，供 dlsym 使用（必须以 '\0' 结尾）
    size_t name_len = end - start;
    char   name[128];
    if (name_len >= sizeof(name)) {
        fprintf(stderr, "Function name too long\n");
        return false;
    }
    memcpy(name, start, name_len);
    name[name_len] = '\0';

    return compile_source(function_def, name) != NULL;
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
    int (*fn)(void) = (int (*)(void))compile_source(wrapper_src, fn_name);
    free(wrapper_src);
    if (fn == NULL) {
        return false;
    }

    // 在 crepl 主进程内直接调用
    *result = fn();
    return true;
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
