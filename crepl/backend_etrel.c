#define _GNU_SOURCE
#include "backend.h"
#include "compiler.h"

#include <elf.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * ET_REL 自加载后端。
 *
 * 流水线（对应 notes/M4.md §8）：
 *   gcc -c -fPIC -> 解析 ELF -> 段布局/拷贝 -> 符号表 -> 重定位 -> 入口登记
 */

/* ================= 全局符号表：name -> 入口地址 ================= */

#define CREPL_MAX_SYMBOLS 1024
#define CREPL_MAX_NAME    64

// 全局符号表项：符号名 -> 入口地址
struct symbol_entry {
    char  name[CREPL_MAX_NAME];
    void *addr;
};

static struct symbol_entry g_symbols[CREPL_MAX_SYMBOLS];
static size_t              g_symbol_count;

// 在全局符号表中查找符号名，返回地址；找不到返回 NULL
// 顺序查找
static void *symbol_lookup(const char *name) {
    for (size_t i = 0; i < g_symbol_count; i++) {
        if (strcmp(g_symbols[i].name, name) == 0) {
            return g_symbols[i].addr;
        }
    }
    return NULL;
}

// 在全局符号表中登记符号名和地址；重名按「后定义覆盖」处理
static bool symbol_define(const char *name, void *addr) {
    if (strlen(name) >= CREPL_MAX_NAME) {
        fprintf(stderr, "etrel: symbol name too long: %s\n", name);
        return false;
    }
    /* 重名按「后定义覆盖」处理（spec 视为 UB） */
    for (size_t i = 0; i < g_symbol_count; i++) {
        if (strcmp(g_symbols[i].name, name) == 0) {
            g_symbols[i].addr = addr;
            return true;
        }
    }
    if (g_symbol_count >= CREPL_MAX_SYMBOLS) {
        fprintf(stderr, "etrel: too many symbols\n");
        return false;
    }
    snprintf(g_symbols[g_symbol_count].name, CREPL_MAX_NAME, "%s", name);
    g_symbols[g_symbol_count].addr = addr;
    g_symbol_count++;
    return true;
}

/* ================= 连续 arena：保证 PC32/PLT32 位移在 ±2GB 内 ================= */

#define CREPL_ARENA_SIZE ((size_t)1 << 30) /* 1 GiB，足够 100 条输入 */

// arena 分配器：在一个连续的 1 GiB 区域内分配内存，保证 PC32/PLT32 位移在 ±2GB 内
static uint8_t *g_arena;
static size_t   g_arena_used;

/* arena 分配器的实现 */
static bool arena_alloc(size_t size, size_t align, void **out) {
    if (g_arena == NULL) {
        // 映射一块连续的 1 GiB 区域，PROT_NONE 避免占用物理内存；后续 mprotect
        // 逐页打开，MAP_NORESERVE 避免过度预留 swap，fd=-1 表示匿名映射
        void *p = mmap(NULL, CREPL_ARENA_SIZE, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED) {
            perror("etrel: mmap arena");
            return false;
        }
        g_arena = p;
    }

    if (align == 0) {
        align = 1;
    }
    // 调整 g_arena_used 以满足对齐要求，并检查是否越界
    size_t start = (g_arena_used + align - 1) & ~(align - 1);
    if (start > CREPL_ARENA_SIZE || size > CREPL_ARENA_SIZE - start) {
        fprintf(stderr, "etrel: arena exhausted\n");
        return false;
    }

    uint8_t *p = g_arena + start;

    /* 实验环境不要求 W^X，直接给 RWX 方便装载后执行 */
    long      page       = sysconf(_SC_PAGESIZE);
    size_t    page_size  = page > 0 ? (size_t)page : 4096;
    uintptr_t page_start = (uintptr_t)p & ~(uintptr_t)(page_size - 1);
    size_t    page_end   = ((uintptr_t)p - page_start + size + page_size - 1) & ~(page_size - 1);
    // mprotect 失败时打印错误并返回 false
    if (mprotect((void *)page_start, page_end, PROT_READ | PROT_WRITE | PROT_EXEC) < 0) {
        perror("etrel: mprotect");
        return false;
    }

    g_arena_used = start + size;
    *out         = p;
    return true;
}

/* ================= ELF 对象文件 ================= */

// ET_REL 可重定位对象文件（即 .o 文件）的内存映射和解析
// 其他类型 ELF 还有 ET_EXEC（可执行文件，静态链接）、ET_DYN（共享库 .so 文件，PIE
// 可执行文件）、ET_CORE（core dump）等
// 符号表、重定位表还存在，必须经过链接装载后分配地址、解析符号、应用重定位才能执行
struct object_file {
    uint8_t *data;
    size_t   size;
};

// ET_REL 镜像：在 arena 内的布局和段地址
struct etrel_image {
    uint8_t  *base;    /* arena 内镜像起始地址 */
    size_t    size;    /* 镜像总大小 */
    uint8_t **section; /* 旧 shndx -> 镜像内地址（非 alloc 段为 NULL） */
};

// 将指定路径的 ET_REL 对象文件映射到内存，返回 object_file；失败返回 false（已打印错误）
static bool map_object(const char *path, struct object_file *obj) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("etrel: open object");
        return false;
    }

    // 读取 ELF 文件头部，检查文件大小是否足够
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < (off_t)sizeof(Elf64_Ehdr)) {
        fprintf(stderr, "etrel: object file is too small\n");
        close(fd);
        return false;
    }

    // 映射 ELF 文件到内存
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        perror("etrel: mmap object");
        return false;
    }
    // 设置 object_file 的 data 和 size
    obj->data = p;
    obj->size = (size_t)st.st_size;
    return true;
}

// 从传入的 object_file 中解析 ELF 头部，返回 Elf64_Ehdr；失败返回 NULL
static const Elf64_Ehdr *object_header(const struct object_file *obj) {
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)obj->data;

    // 检查 ELF 魔数、类、数据编码、类型、机器架构、段表偏移和大小等字段是否合法
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS64 ||
        eh->e_ident[EI_DATA] != ELFDATA2LSB) {
        return NULL;
    }
    if (eh->e_type != ET_REL || eh->e_machine != EM_X86_64) {
        return NULL;
    }
    if (eh->e_shentsize != sizeof(Elf64_Shdr) || eh->e_shoff == 0 || eh->e_shnum == 0) {
        return NULL;
    }
    if (eh->e_shoff > obj->size ||
        (size_t)eh->e_shnum > (obj->size - (size_t)eh->e_shoff) / sizeof(Elf64_Shdr)) {
        return NULL;
    }
    return eh;
}

// 从传入的 object_file 和 ELF 头部中获取段表，返回 Elf64_Shdr 数组；失败返回 NULL
static const Elf64_Shdr *section_headers(const struct object_file *obj, const Elf64_Ehdr *eh) {
    return (const Elf64_Shdr *)(obj->data + eh->e_shoff);
}

// 从传入的 object_file 和段表中获取符号表，返回 Elf64_Sym 数组；失败返回 NULL
static const Elf64_Sym *symbol_table(const struct object_file *obj, const Elf64_Shdr *symsh) {
    if (symsh->sh_type != SHT_SYMTAB || symsh->sh_offset > obj->size ||
        symsh->sh_size > obj->size - symsh->sh_offset) {
        return NULL;
    }
    return (const Elf64_Sym *)(obj->data + symsh->sh_offset);
}

// 从传入的 object_file、ELF 头部和符号表获取符号字符串表，返回字符串数组；失败返回 NULL
static const char *symbol_strings(const struct object_file *obj, const Elf64_Ehdr *eh,
                                  const Elf64_Shdr *symsh) {
    if (symsh->sh_link >= eh->e_shnum) {
        return NULL;
    }
    const Elf64_Shdr *strsh = &section_headers(obj, eh)[symsh->sh_link];
    if (strsh->sh_offset > obj->size || strsh->sh_size > obj->size - strsh->sh_offset) {
        return NULL;
    }
    return (const char *)(obj->data + strsh->sh_offset);
}

/* ================= 镜像布局与拷贝 ================= */
// 从 object_file 和 ELF 头部中构建镜像，返回 etrel_image；失败返回 false（已打印错误）
static bool image_build(const struct object_file *obj, const Elf64_Ehdr *eh,
                        struct etrel_image *img) {
    const Elf64_Shdr *sh = section_headers(obj, eh);

    img->section = calloc(eh->e_shnum, sizeof(*img->section));
    if (img->section == NULL) {
        return false;
    }

    /* 1. 按 sh_addralign 计算总大小 */
    size_t total = 0;
    for (size_t i = 0; i < eh->e_shnum; i++) {
        if ((sh[i].sh_flags & SHF_ALLOC) == 0 || sh[i].sh_size == 0) {
            continue;
        }
        size_t align = sh[i].sh_addralign != 0 ? (size_t)sh[i].sh_addralign : 1;
        total        = (total + align - 1) & ~(align - 1);
        if (sh[i].sh_size > SIZE_MAX - total) {
            return false;
        }
        total += sh[i].sh_size;
    }
    if (total == 0) {
        fprintf(stderr, "etrel: no allocatable sections\n");
        return false;
    }

    /* 2. 在 arena 内分配镜像（arena 按页对齐，内部按段落对齐） */
    void *base = NULL;
    if (!arena_alloc(total, 16, &base)) {
        return false;
    }
    img->base = base;
    img->size = total;

    /* 3. 拷贝内容，.bss（SHT_NOBITS）清零 */
    size_t cursor = 0;
    for (size_t i = 0; i < eh->e_shnum; i++) {
        if ((sh[i].sh_flags & SHF_ALLOC) == 0 || sh[i].sh_size == 0) {
            continue;
        }
        size_t align = sh[i].sh_addralign != 0 ? (size_t)sh[i].sh_addralign : 1;
        cursor       = (cursor + align - 1) & ~(align - 1);

        uint8_t *dst    = img->base + cursor;
        img->section[i] = dst;

        if (sh[i].sh_type == SHT_NOBITS) {
            memset(dst, 0, sh[i].sh_size);
        } else {
            if (sh[i].sh_offset > obj->size || sh[i].sh_size > obj->size - sh[i].sh_offset) {
                fprintf(stderr, "etrel: section %zu out of file bounds\n", i);
                return false;
            }
            memcpy(dst, obj->data + sh[i].sh_offset, sh[i].sh_size);
        }
        cursor += sh[i].sh_size;
    }
    return true;
}

/* ================= 符号解析 ================= */

/* defined -> 镜像内地址；UNDEF -> 全局符号表；不可解析则 ok = false */
static void *symbol_value(const struct object_file *obj, const Elf64_Ehdr *eh,
                          const Elf64_Shdr *symsh, const struct etrel_image *img, size_t index,
                          bool *ok) {
    *ok = false;

    const Elf64_Sym *syms = symbol_table(obj, symsh);
    const char      *strs = symbol_strings(obj, eh, symsh);
    if (syms == NULL || strs == NULL || index >= symsh->sh_size / sizeof(Elf64_Sym)) {
        return NULL;
    }

    const Elf64_Sym *s = &syms[index];

    if (s->st_shndx == SHN_UNDEF) {
        const char *name = strs + s->st_name;
        void       *addr = symbol_lookup(name);
        if (addr == NULL) {
            fprintf(stderr, "etrel: undefined symbol '%s'\n", name);
            return NULL;
        }
        *ok = true;
        return addr;
    }
    if (s->st_shndx == SHN_ABS) {
        *ok = true;
        return (void *)(uintptr_t)s->st_value;
    }
    if (s->st_shndx >= eh->e_shnum || img->section[s->st_shndx] == NULL) {
        return NULL;
    }
    *ok = true;
    return img->section[s->st_shndx] + s->st_value;
}

/* 把本对象定义的所有函数/对象登记到全局符号表，供后续对象解析 */
static bool register_symbols(const struct object_file *obj, const Elf64_Ehdr *eh,
                             const struct etrel_image *img) {
    const Elf64_Shdr *sh = section_headers(obj, eh);

    for (size_t i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_SYMTAB) {
            continue;
        }
        const Elf64_Sym *syms = symbol_table(obj, &sh[i]);
        const char      *strs = symbol_strings(obj, eh, &sh[i]);
        if (syms == NULL || strs == NULL) {
            return false;
        }

        size_t count = sh[i].sh_size / sizeof(Elf64_Sym);
        for (size_t j = 1; j < count; j++) { /* 跳过索引 0 的空符号 */
            const Elf64_Sym *s    = &syms[j];
            unsigned         type = ELF64_ST_TYPE(s->st_info);
            if (type != STT_FUNC && type != STT_OBJECT) {
                continue;
            }
            if (s->st_shndx == SHN_UNDEF || s->st_shndx == SHN_ABS || s->st_shndx >= eh->e_shnum ||
                img->section[s->st_shndx] == NULL) {
                continue;
            }
            const char *name = strs + s->st_name;
            if (name[0] == '\0') {
                continue;
            }
            if (!symbol_define(name, img->section[s->st_shndx] + s->st_value)) {
                return false;
            }
        }
    }
    return true;
}

// 在镜像中查找指定符号名的入口地址；找不到返回 NULL
static void *find_entry(const struct object_file *obj, const Elf64_Ehdr *eh,
                        const struct etrel_image *img, const char *want) {
    const Elf64_Shdr *sh = section_headers(obj, eh);

    for (size_t i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_SYMTAB) {
            continue;
        }
        const Elf64_Sym *syms = symbol_table(obj, &sh[i]);
        const char      *strs = symbol_strings(obj, eh, &sh[i]);
        if (syms == NULL || strs == NULL) {
            continue;
        }

        size_t count = sh[i].sh_size / sizeof(Elf64_Sym);
        for (size_t j = 1; j < count; j++) {
            const Elf64_Sym *s = &syms[j];
            if (ELF64_ST_TYPE(s->st_info) != STT_FUNC || s->st_shndx == SHN_UNDEF ||
                s->st_shndx >= eh->e_shnum || img->section[s->st_shndx] == NULL) {
                continue;
            }
            if (strcmp(strs + s->st_name, want) == 0) {
                return img->section[s->st_shndx] + s->st_value;
            }
        }
    }
    return NULL;
}

/* ================= 重定位 ================= */

/* 写入函数负责边界与溢出检查；place 已限定在目标段内 */
static bool store_u32(uint8_t *place, size_t avail, uint32_t value) {
    if (avail < sizeof(value)) {
        fprintf(stderr, "etrel: relocation out of section bounds\n");
        return false;
    }
    memcpy(place, &value, sizeof(value));
    return true;
}

static bool store_u64(uint8_t *place, size_t avail, uint64_t value) {
    if (avail < sizeof(value)) {
        fprintf(stderr, "etrel: relocation out of section bounds\n");
        return false;
    }
    memcpy(place, &value, sizeof(value));
    return true;
}

static bool store_pc32(uint8_t *place, size_t avail, int64_t value) {
    if (value < INT32_MIN || value > INT32_MAX) {
        fprintf(stderr, "etrel: PC-relative relocation overflow: %lld\n", (long long)value);
        return false;
    }
    return store_u32(place, avail, (uint32_t)(int32_t)value);
}

/* 为 GOTPCREL* 分配一个 8 字节槽并写入符号地址（不做 relaxation） */
static bool make_got_slot(uintptr_t sym, uint8_t **slot_out) {
    void *slot = NULL;
    if (!arena_alloc(sizeof(uint64_t), sizeof(uint64_t), &slot)) {
        return false;
    }
    uint64_t value = (uint64_t)sym;
    memcpy(slot, &value, sizeof(value));
    *slot_out = slot;
    return true;
}

static bool apply_relocations(const struct object_file *obj, const Elf64_Ehdr *eh,
                              const struct etrel_image *img) {
    const Elf64_Shdr *sh = section_headers(obj, eh);

    for (size_t i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_RELA) {
            continue;
        }
        if (sh[i].sh_info >= eh->e_shnum || sh[i].sh_link >= eh->e_shnum) {
            return false;
        }

        const Elf64_Shdr *target = &sh[sh[i].sh_info];
        if ((target->sh_flags & SHF_ALLOC) == 0 || img->section[sh[i].sh_info] == NULL) {
            continue;
        }
        if (sh[i].sh_offset > obj->size || sh[i].sh_size > obj->size - sh[i].sh_offset ||
            sh[i].sh_size % sizeof(Elf64_Rela) != 0) {
            return false;
        }

        const Elf64_Rela *rela  = (const Elf64_Rela *)(obj->data + sh[i].sh_offset);
        size_t            count = sh[i].sh_size / sizeof(Elf64_Rela);

        for (size_t j = 0; j < count; j++) {
            unsigned type  = (unsigned)ELF64_R_TYPE(rela[j].r_info);
            size_t   index = (size_t)ELF64_R_SYM(rela[j].r_info);

            if (rela[j].r_offset > target->sh_size) {
                return false;
            }
            size_t   avail = (size_t)(target->sh_size - rela[j].r_offset);
            uint8_t *place = img->section[sh[i].sh_info] + rela[j].r_offset;

            bool  ok = false;
            void *s  = symbol_value(obj, eh, &sh[sh[i].sh_link], img, index, &ok);
            if (!ok) {
                return false;
            }

            uintptr_t S = (uintptr_t)s;
            uintptr_t P = (uintptr_t)place;
            int64_t   A = (int64_t)rela[j].r_addend;

            switch (type) {
            case R_X86_64_NONE:
                break;

            case R_X86_64_PLT32: /* 函数调用：S + A - P */
            case R_X86_64_PC32:  /* PC 相对引用：S + A - P */
                if (!store_pc32(place, avail, (int64_t)S + A - (int64_t)P)) {
                    return false;
                }
                break;

            case R_X86_64_32: { /* 绝对地址，32 位零扩展 */
                int64_t value = (int64_t)S + A;
                if (value < 0 || (uint64_t)value > UINT32_MAX) {
                    fprintf(stderr, "etrel: R_X86_64_32 overflow: 0x%llx\n",
                            (unsigned long long)value);
                    return false;
                }
                if (!store_u32(place, avail, (uint32_t)value)) {
                    return false;
                }
                break;
            }

            case R_X86_64_32S: { /* 绝对地址，32 位符号扩展 */
                int64_t value = (int64_t)S + A;
                if (value < INT32_MIN || value > INT32_MAX) {
                    fprintf(stderr, "etrel: R_X86_64_32S overflow: %lld\n", (long long)value);
                    return false;
                }
                if (!store_u32(place, avail, (uint32_t)(int32_t)value)) {
                    return false;
                }
                break;
            }

            case R_X86_64_64: /* 绝对地址，64 位 */
                if (!store_u64(place, avail, (uint64_t)((int64_t)S + A))) {
                    return false;
                }
                break;

            case R_X86_64_PC64: /* PC 相对，64 位 */
                if (!store_u64(place, avail, (uint64_t)((int64_t)S + A - (int64_t)P))) {
                    return false;
                }
                break;

            case R_X86_64_GOTPCREL:
#ifdef R_X86_64_GOTPCRELX
            case R_X86_64_GOTPCRELX:
#endif
#ifdef R_X86_64_REX_GOTPCRELX
            case R_X86_64_REX_GOTPCRELX:
#endif
            {
                /* 不实现 relaxation：分配真实 GOT 槽，位移指向该槽 */
                uint8_t *slot = NULL;
                if (!make_got_slot(S, &slot)) {
                    return false;
                }
                if (!store_pc32(place, avail, (int64_t)(uintptr_t)slot + A - (int64_t)P)) {
                    return false;
                }
                break;
            }

            default:
                fprintf(stderr, "etrel: unsupported relocation type %u\n", type);
                return false;
            }
        }
    }
    return true;
}

/* ================= 后端入口 ================= */

void *crepl_backend_etrel_load(const char *source, const char *symbol) {
    char c_path[64];
    // 先把源代码写入临时文件，再编译成 .o 文件
    if (!crepl_write_source(source, c_path, sizeof(c_path))) {
        return NULL;
    }
    // 编译成 .o 文件
    char o_path[sizeof(c_path) + 4];
    snprintf(o_path, sizeof(o_path), "%s.o", c_path);

    if (!crepl_compile(c_path, o_path, false)) {
        unlink(c_path);
        return NULL;
    }
    unlink(c_path); /* 编译完成，源码文件可以删除 */

    // 读取并解析 .o 文件
    struct object_file obj   = {0};
    struct etrel_image img   = {0};
    void              *entry = NULL;

    if (!map_object(o_path, &obj)) {
        goto out;
    }

    // 解析 ELF 头部，检查是否为有效的 x86-64 ET_REL 对象文件
    const Elf64_Ehdr *eh = object_header(&obj);
    if (eh == NULL) {
        fprintf(stderr, "etrel: not a valid x86-64 ET_REL object\n");
        goto out;
    }

    // 将读取的对象文件构建为镜像映射到 arena 内存中，并按段落对齐
    if (!image_build(&obj, eh, &img)) {
        goto out;
    }
    /* 先确保重定位成功，再把本对象符号暴露给后续对象，避免失败镜像污染全局表 */
    if (!apply_relocations(&obj, eh, &img)) {
        goto out;
    }
    if (!register_symbols(&obj, eh, &img)) {
        goto out;
    }

    entry = find_entry(&obj, eh, &img, symbol);
    if (entry == NULL) {
        fprintf(stderr, "etrel: entry symbol '%s' not found\n", symbol);
        goto out;
    }
    if (!symbol_define(symbol, entry)) {
        entry = NULL;
    }

out:
    free(img.section);
    if (obj.data != NULL) {
        munmap(obj.data, obj.size);
    }
    unlink(o_path);
    return entry;
}
