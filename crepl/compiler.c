#define _GNU_SOURCE
#include "compiler.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

bool crepl_write_all(int fd, const char *buf, size_t len) {
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

/* fork + exec gcc：stderr 收集到临时日志，失败时回显；子进程必须 _exit */
static bool spawn_gcc(char *const argv[]) {
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
        /* mkstemp 可能恰好返回 2，此时 fd 2 已指向日志，不能再 close */
        if (err_fd != STDERR_FILENO) {
            dup2(err_fd, STDERR_FILENO);
            close(err_fd);
        }
        execvp("gcc", argv);
        _exit(127); /* exec 成功不会返回 */
    }

    /* 父进程关闭自己的 fd 副本，之后按文件名重新打开读取 */
    close(err_fd);

    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("Failed to wait for gcc");
        unlink(err_path);
        return false;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "Compilation failed\n");
        FILE *err_file = fopen(err_path, "r");
        if (err_file != NULL) {
            char buf[512];
            while (fgets(buf, sizeof(buf), err_file) != NULL) {
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

bool crepl_write_source(const char *source, char *path, size_t path_size) {
    char tmpl[] = "/tmp/crepl_XXXXXX.c";
    int  fd     = mkstemps(tmpl, 2);
    if (fd < 0) {
        perror("Failed to create temporary file");
        return false;
    }

    if (!crepl_write_all(fd, source, strlen(source)) || !crepl_write_all(fd, "\n", 1)) {
        perror("Failed to write to temporary file");
        close(fd);
        unlink(tmpl);
        return false;
    }
    close(fd);

    if (strlen(tmpl) + 1 > path_size) {
        fprintf(stderr, "Temporary path too long\n");
        unlink(tmpl);
        return false;
    }
    snprintf(path, path_size, "%s", tmpl);
    return true;
}

bool crepl_compile(const char *c_path, const char *out_path, bool shared) {
    /* 共享库路线：常规 dlopen 用 */
    char *const shared_args[] = {"gcc",
                                 "-x",
                                 "c",
                                 "-shared",
                                 "-fPIC",
                                 "-Wno-implicit-function-declaration",
                                 "-o",
                                 (char *)out_path,
                                 (char *)c_path,
                                 NULL};

    /* ET_REL 路线：需要 PIC 重定位；关掉 unwind/stack protector，避免额外依赖 */
    char *const object_args[] = {"gcc",
                                 "-x",
                                 "c",
                                 "-c",
                                 "-fPIC",
                                 "-O0",
                                 "-fno-asynchronous-unwind-tables",
                                 "-fno-unwind-tables",
                                 "-fno-stack-protector",
                                 "-Wno-implicit-function-declaration",
                                 "-o",
                                 (char *)out_path,
                                 (char *)c_path,
                                 NULL};

    return spawn_gcc(shared ? shared_args : object_args);
}
