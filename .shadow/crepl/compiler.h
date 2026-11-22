#ifndef CREPL_COMPILER_H
#define CREPL_COMPILER_H

#include <stdbool.h>
#include <stddef.h>

/*
 * 把 source 写入唯一的临时 .c 文件，路径写入 path 指向的缓冲区。
 * 成功返回 true；失败返回 false（已打印错误）。
 */
bool crepl_write_source(const char *source, char *path, size_t path_size);

/*
 * 调用 gcc 编译 c_path：
 *   shared = true  -> gcc -shared -fPIC，产出共享库
 *   shared = false -> gcc -c -fPIC，产出可重定位目标文件（ET_REL）
 * 编译错误收集到临时日志，失败时回显到 stderr。
 */
bool crepl_compile(const char *c_path, const char *out_path, bool shared);

/* 循环 write，处理部分写与 EINTR */
bool crepl_write_all(int fd, const char *buf, size_t len);

#endif
