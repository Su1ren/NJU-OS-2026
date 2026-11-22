#include "backend.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 后端选择：默认 dlopen；设置 CREPL_BACKEND=etrel 可切到自加载路线。
 * 只在第一次调用时读取环境变量，之后沿用结果。
 */
void *crepl_backend_load(const char *source, const char *symbol) {
    static int  selected;
    static bool use_etrel;

    if (!selected) {
        const char *env = getenv("CREPL_BACKEND");
        use_etrel       = (env != NULL && strcmp(env, "etrel") == 0);
        if (env != NULL && !use_etrel && strcmp(env, "dlopen") != 0) {
            fprintf(stderr, "Unknown CREPL_BACKEND '%s'; falling back to dlopen\n", env);
        }
        selected = 1;
    }

    return use_etrel ? crepl_backend_etrel_load(source, symbol)
                     : crepl_backend_dlopen_load(source, symbol);
}
