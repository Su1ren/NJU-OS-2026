#include "backend.h"
#include "compiler.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_LOADED 512 /* 保留已加载库的 handle，避免被卸载 */

static void  *loaded_handles[MAX_LOADED];
static size_t n_loaded;

/*
 * dlopen 路线：
 *   写 .c -> gcc -shared 产出唯一路径的 .so -> dlopen(RTLD_NOW | RTLD_GLOBAL)
 *   -> dlsym -> 保留 handle（不 dlclose）
 * RTLD_GLOBAL 让本库符号进入全局作用域，后续库才能解析到先前定义的函数。
 */
void *crepl_backend_dlopen_load(const char *source, const char *symbol) {
    char c_path[64];
    if (!crepl_write_source(source, c_path, sizeof(c_path))) {
        return NULL;
    }

    /* 唯一的 so 路径：从 .c 路径派生，避免 dlopen 命中旧缓存 */
    char so_path[sizeof(c_path) + 4];
    snprintf(so_path, sizeof(so_path), "%s.so", c_path);

    if (!crepl_compile(c_path, so_path, true)) {
        unlink(c_path);
        return NULL;
    }
    unlink(c_path); /* 编译完成，源码文件可以删除 */

    void *handle = dlopen(so_path, RTLD_NOW | RTLD_GLOBAL);
    if (handle == NULL) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        unlink(so_path);
        return NULL;
    }
    unlink(so_path); /* 映射已建立，文件可以删除 */

    dlerror(); /* 清除已有错误状态 */
    void       *sym = dlsym(handle, symbol);
    const char *err = dlerror();
    if (err != NULL || sym == NULL) {
        fprintf(stderr, "dlsym: %s\n", err != NULL ? err : "symbol not found");
        dlclose(handle);
        return NULL;
    }

    if (n_loaded < MAX_LOADED) {
        loaded_handles[n_loaded++] = handle;
    }
    return sym;
}
