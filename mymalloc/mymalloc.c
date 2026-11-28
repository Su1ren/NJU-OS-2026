#include "mymalloc.h"
#include <stdint.h>

spinlock_t big_lock; // 全局锁，所有线程竞争同一个锁

static shard_t shards[NSHARD]; // 全局的 shard 分片数组，存储了 NCLASS 个小块的空闲链表
static pool_t pools[NCLASS];

// We don't need this malloc_count. You can remove it.
// 暂时保留：tests/tests-trivial.c 的 concurrent 用例 extern 引用它，
// 删掉会链接失败（步骤 6 清理时连同用例一起处理）。
long malloc_count;

/**
 * @brief 将 payload 压入空闲链表 head 的头部
 *
 * @param head 原始空闲链表头指针的地址
 * @param payload chunk 的 payload 指针，指向空闲块的起始位置
 */
static void list_push(free_node_t **head, void *payload) {
    if (payload == NULL) {
        return;
    }
    free_node_t *node = (free_node_t *)payload;
    node->next        = *head;
    *head             = node;
}

/**
 * @brief 从空闲链表 head 弹出一个节点，并返回其 payload 指针
 *
 * @param head 空闲链表头指针的地址
 * @return void* 返回弹出节点的 payload 指针，如果链表为空则返回 NULL
 */
static void *list_pop(free_node_t **head) {
    // 空链表返回 NULL
    if (*head == NULL) {
        return NULL;
    }
    // 取出第一个节点
    free_node_t *node = *head;
    *head             = node->next;
    return (void *)node;
}

/**
 * @brief 从链表取出 want 个节点，返回实际取出的数量，并将取出的节点组成的新链表头指针存储在
 * taken_head 中
 *
 * @param head 空闲链表头指针的地址
 * @param want 想要取出的节点数量
 * @param taken_head 取出的节点组成的新链表头指针的地址
 * @return uint32_t 实际取出的节点数量
 */
static uint32_t list_take(free_node_t **head, uint32_t want, free_node_t **taken_head) {
    uint32_t count = 0;
    *taken_head    = NULL;
    // 从链表中取出 want 个节点
    while (*head && count < want) {
        free_node_t *node = *head;
        *head             = node->next;
        node->next        = *taken_head;
        *taken_head       = node;
        ++count;
    }
    return count;
}

/**
 * @brief 把一条已经串好的空闲链 chain 整体接到 *head 的前面
 * @note list_push 只接受单个节点：它会把 node->next 改写成 *head，
 *       所以直接 push 一条链的头节点会把链的其余部分全部丢掉。
 *       这里走一遍找 tail，只在新建 chunk 时发生（每 chunk 一次，nobj ≤ 4092）。
 * @param head 目标空闲链表头指针的地址
 * @param chain 待拼接的链，可以为 NULL
 */
static void list_splice(free_node_t **head, free_node_t *chain) {
    if (chain == NULL) {
        return;
    }
    // 先走到链尾
    free_node_t *tail = chain;
    while (tail->next) {
        tail = tail->next;
    }
    // 接上
    tail->next = *head;
    *head      = chain;
}

/**
 * @brief 把已经 vmalloc 的 [base, base + CHUNK_SIZE) 的内存区域切成 nobj 个块，填充 chunk_hdr，所有
 * payload 串成一个空闲链表，返回链表头指针和实际切出的块数
 *
 * @param base vmalloc 返回的内存区域的起始地址
 * @param class_id 该 chunk 所属的 class_id
 * @param out 返回的空闲链表头指针
 * @param n 返回的实际切出的块数
 * @return int 0 表示成功，-1 表示失败
 */
static int chunk_init(void *base, unsigned class_id, free_node_t **out, uint32_t *n) {
    // 校验 base 地址页对齐
    if ((uintptr_t)base % PAGE_SIZE || cap_of(class_id) == 0 || block_size_of(class_id) == 0) {
        return -1;
    }

    // 计算每个块的大小
    size_t block_size = block_size_of(class_id);
    // 计算可以切出的块数
    uint32_t nobj = (CHUNK_SIZE - CHUNK_HDR_SIZE) / block_size;
    // 初始化 chunk_hdr
    chunk_hdr_t *hdr = (chunk_hdr_t *)base;
    hdr->class_id    = class_id;
    hdr->nobj        = nobj;
    hdr->magic       = CHUNK_MAGIC;
    hdr->self        = (uintptr_t)base;
    hdr->block_size  = cap_of(class_id) + sizeof(small_hdr_t);
    // 初始化空闲链表
    *out = NULL;
    // 头插法
    for (uint32_t i = 0; i < nobj; i++) {
        char *block         = (char *)base + CHUNK_HDR_SIZE + i * block_size;
        *(uintptr_t *)block = (uintptr_t)hdr | (class_id + 1); // 设置块头的 tag
        list_push(out, block + sizeof(small_hdr_t));           // payload 指针 = block + 8
    }
    *n = nobj;
    return 0;
}

/**
 * @brief vmalloc 一个 chunk 并调用 chunk_init 初始化，返回链表头指针和实际切出的块数
 *
 * @param class_id 指定的 class_id
 * @param out 切出的空闲链表头指针
 * @param n 实际切出的块数
 * @return int 0 表示成功，-1 表示失败，失败时 out 和 n 不会被修改
 */
static int chunk_create(unsigned class_id, free_node_t **out, uint32_t *n) {
    // 调用 vmalloc 分配一块内存
    void *base = vmalloc(NULL, CHUNK_SIZE);
    if (base == NULL) {
        return -1;
    }
    // 调用 chunk_init 初始化
    int ret = chunk_init(base, class_id, out, n);
    if (ret != 0) {
        // 初始化失败，释放内存
        vmfree(base, CHUNK_SIZE);
        return -1;
    }
    return 0;
}

/**
 * @brief 分配指定大小的大块内存，返回指向 payload 的指针
 *
 * @param size 申请的内存大小
 * @return void* 指向 payload 的指针，如果分配失败返回 NULL
 */
static void *alloc_large(size_t size) {
    // 计算需要 vmalloc 的长度，包含 large_hdr_t 的大小
    // 向上对齐到 PAGE_SIZE
    size_t len  = ALIGN_UP(size + sizeof(large_hdr_t), PAGE_SIZE);
    void  *base = vmalloc(NULL, len);
    if (base == NULL) {
        return NULL;
    }
    // 初始化 large_hdr_t
    large_hdr_t *hdr = (large_hdr_t *)base;
    hdr->length      = len;
    hdr->magic       = LARGE_MAGIC;
    // 返回 payload 指针
    return (char *)base + sizeof(large_hdr_t);
}

/**
 * @brief 释放大块内存，ptr 指向 payload，实际释放的内存区域是 [base, base + hdr->length)
 *
 * @param ptr 指向 payload 的指针
 */
static void free_large(void *ptr) {
    void        *base = (char *)ptr - sizeof(large_hdr_t);
    large_hdr_t *hdr  = (large_hdr_t *)base;
    // 校验 magic
    if (hdr->magic != LARGE_MAGIC) {
        // 错误的 free，忽略
        return;
    }
    vmfree(base, hdr->length);
}

/**
 * @brief 分配指定 class_id 的小块内存，返回指向 payload 的指针
 *
 * @param class_id 小块内存所属的 class_id
 * @return void* 指向 payload 的指针，如果分配失败返回 NULL
 */
static void *alloc_small(unsigned class_id) {
    // 持有 big_lock，保证线程安全
    spin_lock(&big_lock);
    // 从 class 所属 pool 的空闲链表中取出一个块
    void *p = list_pop(&pools[class_id].head);
    if (p) {
        // 只有真正取到块才减计数：否则 uint32_t 会从 0 下溢成 UINT32_MAX
        pools[class_id].n--;
    }
    spin_unlock(&big_lock);
    if (p) {
        // 成功分配
        return p;
    }
    // 没有空闲块，则额外 vmalloc 一个 chunk 并初始化
    free_node_t *new_head = NULL;
    uint32_t     n        = 0;
    if (chunk_create(class_id, &new_head, &n) != 0) {
        // chunk_create 失败，释放 big_lock
        return NULL;
    }
    // 将新创建的空闲链表头指针赋值给 out
    // 持锁
    spin_lock(&big_lock);
    // 将新创建的空闲链表加入 pool 的空闲链表
    // 注意：不能写 list_push(&pools[class_id].head, new_head)，那只会保留头节点，
    // 把新 chunk 剩下的 nobj - 1 个块全部丢掉（每一个块都对应一个 chunk 的话，
    // 内存会被瞬间吃光）
    list_splice(&pools[class_id].head, new_head);
    pools[class_id].n += n;
    // 再次尝试分配一个返回
    p = list_pop(&pools[class_id].head);
    if (p) {
        pools[class_id].n--;
    }
    spin_unlock(&big_lock);
    return p;
}

static void free_small(void *ptr, unsigned class_id) {
    // 持有 big_lock，保证线程安全
    spin_lock(&big_lock);
    // 将 ptr 压入 class 所属 pool 的空闲链表
    list_push(&pools[class_id].head, ptr);
    pools[class_id].n++;
    spin_unlock(&big_lock);
}

/**
 * @brief 多处理器下安全的 malloc 实现
 * @note 用根据申请大小选择不同的分配策略，使用自旋锁保证线程安全
 * @param size 待分配的内存大小
 * @return void* 分配成功返回指向内存块的指针，失败返回 NULL
 */
void *mymalloc(size_t size) {
    // 大块直接向 vmalloc 要整段（无锁、可并行）；小块走 size class 的 pool
    void *p = (size > LARGE_BLOCK_THRESHOLD) ? alloc_large(size) : alloc_small(class_of(size));
    if (p) {
        // 调试计数，concurrent 用例依赖它（步骤 6 清理时连用例一起删掉）
        __atomic_fetch_add(&malloc_count, 1, __ATOMIC_RELAXED);
    }
    return p;
}

/**
 * @brief 释放 mymalloc 返回的指针
 * @note ptr 必须是某次分配返回的左端点；否则是未定义行为
 * @param ptr 待释放的内存块的 payload 起始地址
 */
void myfree(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    // 块头就在 ptr 之前 8B：tag == 0 表示大块，tag == class + 1 表示小块
    uintptr_t hdr = *(uintptr_t *)((char *)ptr - sizeof(small_hdr_t));
    uint32_t  tag = (uint32_t)(hdr & TAG_MASK);
    if (tag == 0) {
        free_large(ptr);
    } else if (tag <= NCLASS) {
        free_small(ptr, tag - 1);
    }
    // tag > NCLASS 说明块头已被踩坏，直接丢弃，避免越界访问 pools[]
}
