#ifndef CREPL_BACKEND_H
#define CREPL_BACKEND_H

/*
 * 代码加载后端接口。
 *
 * 把一段 C 源码编译并加载进当前进程，返回 symbol 的入口地址；
 * 失败返回 NULL（已打印错误）。求值必须发生在主进程内。
 */
void *crepl_backend_load(const char *source, const char *symbol);

/* dlopen 后端：gcc -shared + dlopen(RTLD_NOW | RTLD_GLOBAL) + dlsym */
void *crepl_backend_dlopen_load(const char *source, const char *symbol);

/* ET_REL 后端：gcc -c 产出 .o，进程内自行装载、符号解析与重定位 */
void *crepl_backend_etrel_load(const char *source, const char *symbol);

#endif
