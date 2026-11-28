#include <assert.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

// 自旋锁
typedef struct {
    atomic_int status; // 0 表示未锁定，1 表示已锁定
} spinlock_t;

#define LOCKED    1
#define UNLOCKED  0
#define PAGE_SIZE 4096
// align 必须是 2 的幂。必须先把 align 提升到 size_t：若 align 是 unsigned 常量，
// ~((align) - 1) 在 32 位里求值后零扩展，会把地址/长度的高 32 位清零
// （实测 ALIGN_UP(0x123456789abc, 4096u) == 0x5678a000）。
#define ALIGN_UP(size, align) (((size) + (size_t)(align) - 1) & ~((size_t)(align) - 1))
#define LARGE_BLOCK_THRESHOLD 8192u // 大块内存分配的阈值，超过该值使用大块分配策略
#define MIN_BLOCK             16u // 最小分配块大小，8B 对齐，至少 16B（8B 用于存储 chunk_base | (class+1)）
#define BIN_CAP               32u // 每个 shard 的 bin 的最大容量，超过该容量的空闲块会被释放到全局池中

// 按照大小分块申请的内存
// 一块内存只能来自大块直接获取，或者来自 chunk 中的小块
// 小块的 chunk（vmalloc 得到，64 KiB，页对齐）
// 偏移 0        +---------------------------+
//               | chunk_hdr_t (64 B)        |   magic / class / block_size / nobj / self
// 偏移 64       +---------------------------+
//               | 8B hdr | payload (cap B)  |   block 0
//               +---------------------------+
//               | 8B hdr | payload (cap B)  |   block 1
//               ...                          |
//               +---------------------------+
//               | 尾部 < block_size 的浪费    |
// 偏移 65536    +---------------------------+

// 单个小块：   [ hdr: chunk_base | (class+1) ][ payload ... ]
//                                         ^ 返回给用户的 ptr（8 字节对齐）

// 大块：       [ length (8B) ][ magic (8B) ][ payload ... ]
//                                 ^ ptr-8 = magic，低 12 位为 0

// 小块内存头 hdr：8B
#define CHUNK_MASK (~(uintptr_t)(PAGE_SIZE - 1))
#define TAG_MASK   0xFFFu // chunk 基地址按 4096B 对齐，低 12 位可用于存储标记
#define NCLASS     40

typedef struct {
    uintptr_t hdr; // chunk_base | (class+1)；保留给大块的是 tag==0，不是 class 0
} small_hdr_t;

_Static_assert(sizeof(small_hdr_t) == 8, "small_hdr_t size must be 8 bytes");
_Static_assert(MIN_BLOCK == sizeof(small_hdr_t) + 8, "最小块 = 8B 头 + 8B 最小 payload");
_Static_assert(NCLASS < 4095, "class + 1 必须能够存储在 12 位中");

// ---- 尺寸档位：正向（size -> class）与反向（class -> cap / block_size）----
// 正向：对任意 size_t 都有定义；0 按最小档处理，> 8192 视为调用方漏了大块分流。
static inline unsigned class_of(size_t x) {
    if (x == 0)
        return 0; // malloc(0) 落到 class 0（cap = 8，block = 16）
    if (x <= 128)
        return (unsigned)((x + 7) >> 3) - 1;
    unsigned e    = 63 - __builtin_clzll(x); // floor(log2 x)
    unsigned step = 1u << (e - 2);
    unsigned cap  = (unsigned)((x + step - 1) & ~(size_t)(step - 1));
    unsigned c    = 16 + 4 * (e - 7) + (cap / step - 5);
    if (c >= NCLASS)
        __builtin_trap(); // x > 8192：调用方应先走 vmalloc 大块路径
    return c;
}

// 反向 1：class -> payload 容量 cap
static inline unsigned cap_of(unsigned c) {
    if (c < 16)
        return (c + 1) * 8;
    unsigned e    = 7 + (c - 16) / 4;
    unsigned step = 1u << (e - 2);
    return ((c - 16) % 4 + 5) * step;
}

// 反向 2：class -> 块步长（含 8B 块头），用于 nobj = (CHUNK_SIZE - CHUNK_HDR_SIZE) / block_size
static inline unsigned block_size_of(unsigned c) {
    return cap_of(c) + (unsigned)sizeof(small_hdr_t);
}

// 大块内存头：16B
// myfree 把 ptr-8 处的整个 8 字节当作 tag 解码（tag == 0 即大块），
// 所以 magic 的低 12 位必须为 0；高位只是签名，可任取，但要求非 0 且固定。
// 小端下 hexdump 显示为 00 00 N E R I U S（"SUIREN" 反向）。
#define LARGE_MAGIC 0x53554952454E0000ull // "SUIREN" + 两个 \0

typedef struct {
    size_t    length; // vmalloc 的页对齐长度
    uintptr_t magic;  // 大块内存的魔数，低 12 位为 0
} large_hdr_t;        // 16B，用户指针 = base + 16，16B 对齐

_Static_assert((TAG_MASK + 1) == PAGE_SIZE, "tag 位宽必须等于页内偏移位宽");
_Static_assert((LARGE_MAGIC & TAG_MASK) == 0, "LARGE_MAGIC 低 12 位必须为 0");
_Static_assert(LARGE_MAGIC != 0, "LARGE_MAGIC 不能为 0，否则无法校验");
_Static_assert(sizeof(large_hdr_t) == 16, "large_hdr_t size must be 16 bytes");

// uintptr_t h   = *(uintptr_t *)((char *)ptr - 8);
// uint32_t  tag = (uint32_t)(h & TAG_MASK);
/* tag == 0 → 大块（base = ptr-16, len = hdr->length）
   tag >= 1 → 小块，class = tag - 1，chunk = h & CHUNK_MASK */

// chunk 头：分配在堆
#define CHUNK_SIZE     65536u     // 64 KiB
#define CHUNK_HDR_SIZE 64u        // chunk_hdr_t 的大小，64B
#define CHUNK_MAGIC    0xDEADBEEF // chunk_hdr_t 的魔数

typedef struct {
    uint32_t  magic; // 魔数，固定值 0xDEADBEEF
    uint32_t  class_id;
    uint32_t  block_size; // 步长 = cap + 8（含 8B 块头）；chunk 只服务小块
    uint32_t  nobj;       // 本 chunk 切出的块数 = (CHUNK_SIZE - CHUNK_HDR_SIZE) / block_size
    uintptr_t self;       // chunk 的基地址，低 12 位为 0
    uintptr_t _pad[(CHUNK_HDR_SIZE - 24) / 8]; // 填充到 64B
} chunk_hdr_t;

_Static_assert(sizeof(chunk_hdr_t) == CHUNK_HDR_SIZE, "chunk_hdr_t size must be 64 bytes");
_Static_assert(CHUNK_HDR_SIZE <= PAGE_SIZE, "chunk 头必须落在第一个页内");
_Static_assert(CHUNK_SIZE % PAGE_SIZE == 0, "vmalloc 要求长度是 PAGE_SIZE 的整数倍");
_Static_assert(CHUNK_SIZE > CHUNK_HDR_SIZE, "chunk 至少要能切出一块");

// 用单链表串联空闲块
// 空闲链表节点复用 payload 的前 8B，存储 next 指针
// 即空闲数据本身
typedef struct free_node {
    struct free_node *next; // 写在 payload 的前 8B
} free_node_t;

// 一个（shard，class）的本地缓存，存储了该 class 的空闲块链表
typedef struct {
    free_node_t *head; // 空闲链表头，指向某块的 payload 起点
    uint32_t     n;    // 当前缓存数量，可以在 fast path 中判断上限
    uint32_t     _pad; // 填充到 16B
} bin_t;               // 16B

// shard(分片) 是 mymalloc 的核心数据结构，存储了 NCLASS 个小块的空闲链表
typedef struct {
    bin_t bins[NCLASS]; // 小块的空闲链表数组
    spinlock_t lock; // 保护 bins 的自旋锁，同一 shard 被两个 tid 同时访问时需要加锁
} shard_t;           // 16B * NCLASS + 4B + 4B = 648B

// pool_t 是 mymalloc 的顶层数据结构
typedef struct { // 每个 class 的全局池（slow path）
    free_node_t *head;
    uint32_t     n;
    uint32_t     _pad;
    spinlock_t   lock; // 保护该 class 全局空闲链的自旋锁（slow path）
} pool_t;              // 16B + 4B + 4B = 24B

#define NSHARD 16u // shard 的数量，必须是 2 的幂

// 锁定传入的自旋锁，属于乐观锁，可能会自旋等待
static inline void spin_lock(spinlock_t *lock) {
    int expected;
    do {
        expected = UNLOCKED;
    } while (!atomic_compare_exchange_strong(&lock->status, &expected, LOCKED));
}

// 解锁传入的自旋锁
static inline void spin_unlock(spinlock_t *lock) {
    atomic_store_explicit(&lock->status, UNLOCKED, memory_order_release);
}

// 自实现的多线程安全的 malloc 和 free 函数
void *mymalloc(size_t size);
void  myfree(void *ptr);

// 可以调用这两个函数用于实现 mymalloc 和 myfree 的底层内存分配和释放
void *vmalloc(void *addr, size_t length);
void  vmfree(void *addr, size_t length);

// 注意：list_* / chunk_* / alloc_* / free_* 都是 mymalloc.c 的内部实现，
// 声明留在 .c 里。若写到本头文件，start.c / tests/*.c 会各自看到一组
// “声明了 static 但未定义”的函数，触发 -Wunused-function 而无法链接。
