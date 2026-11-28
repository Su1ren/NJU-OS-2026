#include "mymalloc.h"

#ifdef FREESTANDING

// This checks whether your code works for
// freestanding environments.

void _start() {
}

int main() {
    return 0;
}

#else

#include <sys/mman.h>

/**
 * @brief 分配指定长度的内存块，返回指向该内存块的指针
 *
 * @param addr 内存区域的起始地址，如果为 NULL，则由系统选择地址
 * @param length 内存区域长度，按照页大小（4096 字节）对齐
 * @return void* 分配成功返回指向内存块的指针，失败返回 NULL
 */
void *vmalloc(void *addr, size_t length) {
    // length must be aligned to page size (4096).
    void *result = mmap(addr, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (result == MAP_FAILED) {
        return NULL;
    }
    return result;
}

/**
 * @brief 释放指定长度的内存块
 *
 * @param addr 释放的内存块的起始地址
 * @param length 需要释放的内存块的长度，按照页大小（4096 字节）对齐
 */
void vmfree(void *addr, size_t length) {
    munmap(addr, length);
}

#endif
