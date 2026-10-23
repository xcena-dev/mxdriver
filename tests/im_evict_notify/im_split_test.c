// A parked THP whose PMD is split by a partial munmap, mprotect or mremap must still be paged in.
// Needs a registered provider over the node's memory (IM enabled on mx_dma0) and the test hooks.
// Usage (root): im_split_test <numa-node>
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define THP_SIZE (2UL << 20)
#define PAGE_4K 4096UL
#define TAIL_OFF (THP_SIZE - PAGE_4K)
#define DBG "/sys/kernel/debug/im_soft_fault/"
#define MXIM_ENABLE "/sys/class/mx_dma/mx_dma0_ioctl/im/enable"
#define MAGIC 0x3c3cc3c3a5a55a5aULL
#define MPOL_BIND 2
#define KPF_COMPOUND_HEAD 15
#define KPF_THP 22
#define RESUME_TIMEOUT_S 10.0

enum split_op
{
    OP_NONE,
    OP_MUNMAP,
    OP_MPROTECT,
    OP_MREMAP,
};

static const char* const op_names[] = {"no split", "munmap", "mprotect", "mremap"};
static int passed, failed;

static void check(const char* name, int ok, const char* fmt, ...)
{
    char detail[256] = "";
    if (fmt != NULL)
    {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(detail, sizeof(detail), fmt, ap);
        va_end(ap);
    }
    printf("[%s] %s%s%s\n", ok ? "PASS" : "FAIL", name, detail[0] ? " — " : "", detail);
    fflush(stdout);
    if (ok)
        passed++;
    else
        failed++;
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static uint64_t pagemap_entry(void* addr)
{
    int fd = open("/proc/self/pagemap", O_RDONLY);
    uint64_t entry = 0;
    if (pread(fd, &entry, sizeof(entry), ((uintptr_t)addr / PAGE_4K) * sizeof(entry)) != sizeof(entry))
        entry = 0;
    close(fd);
    return entry;
}

static uint64_t kpageflags(uint64_t pfn)
{
    int fd = open("/proc/kpageflags", O_RDONLY);
    uint64_t flags = 0;
    if (pread(fd, &flags, sizeof(flags), pfn * sizeof(flags)) != sizeof(flags))
        flags = 0;
    close(fd);
    return flags;
}

static int dbg_write(const char* file, uint64_t pfn)
{
    char path[128], buf[32];
    snprintf(path, sizeof(path), DBG "%s", file);
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -errno;
    int len = snprintf(buf, sizeof(buf), "%#lx", (unsigned long)pfn);
    int ret = write(fd, buf, len) == len ? 0 : -errno;
    close(fd);
    return ret;
}

// Reads a counter from the first line of the status file.
static long status_value(const char* key)
{
    char buf[4096], pattern[64];
    int fd = open(DBG "status", O_RDONLY);
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    buf[n > 0 ? n : 0] = '\0';
    snprintf(pattern, sizeof(pattern), " %s=", key);
    char* p = strstr(buf, pattern);
    if (p == NULL && strncmp(buf, pattern + 1, strlen(pattern) - 1) == 0)
        p = buf - 1;
    return p ? atol(p + strlen(pattern)) : -1;
}

static int parked(uint64_t pfn)
{
    char buf[4096], key[64];
    int fd = open(DBG "status", O_RDONLY);
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    buf[n > 0 ? n : 0] = '\0';
    snprintf(key, sizeof(key), "pfn=%#lx ", (unsigned long)pfn);
    return strstr(buf, key) != NULL;
}

static uint64_t word_at(int idx, size_t off)
{
    return (off / 8) ^ (MAGIC + (uint64_t)idx);
}

static int verify(volatile uint64_t* base, int idx, size_t from, size_t to)
{
    for (size_t off = from; off < to; off += 8)
        if (base[off / 8] != word_at(idx, off))
            return 0;
    return 1;
}

struct reader
{
    volatile uint64_t* addr;
    uint64_t value;
    atomic_int done;
    double t_start, t_end;
};

static void* read_thread(void* arg)
{
    struct reader* r = arg;
    r->t_start = now();
    r->value = *r->addr;
    r->t_end = now();
    atomic_store(&r->done, 1);
    return NULL;
}

// Maps a 2 MiB-aligned THP on @node filled with the pattern of case @idx; returns its head pfn or 0.
static uint64_t make_thp(int node, int idx, uint8_t** raw, uint64_t** base)
{
    unsigned long mask = 1UL << node;
    *raw = mmap(NULL, 2 * THP_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    *base = (uint64_t*)(((uintptr_t)*raw + THP_SIZE - 1) & ~(THP_SIZE - 1));
    syscall(SYS_mbind, *base, THP_SIZE, MPOL_BIND, &mask, sizeof(mask) * 8, 0);
    madvise(*base, THP_SIZE, MADV_HUGEPAGE);
    for (size_t off = 0; off < THP_SIZE; off += 8)
        (*base)[off / 8] = word_at(idx, off);
    uint64_t pfn = pagemap_entry(*base) & ((1ULL << 55) - 1);
    uint64_t kf = kpageflags(pfn);
    int is_thp = (kf >> KPF_THP & 1) && (kf >> KPF_COMPOUND_HEAD & 1) && (pfn % 512 == 0);
    return is_thp ? pfn : 0;
}

static void run_case(int node, enum split_op op)
{
    const char* name = op_names[op];
    char label[128];
    uint8_t* raw;
    uint64_t* base;
    uint64_t pfn = make_thp(node, op, &raw, &base);
    snprintf(label, sizeof(label), "%s: the region is a PMD-mapped THP", name);
    check(label, pfn != 0, "pfn=%#lx", (unsigned long)pfn);
    if (pfn == 0)
        return;

    uint8_t* dst = mmap(NULL, PAGE_4K, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    long fetches0 = status_value("fetches");
    long restored0 = status_value("restored");
    int r = dbg_write("quiesce", pfn);
    snprintf(label, sizeof(label), "%s: quiesce parks the THP", name);
    check(label, r == 0 && !(pagemap_entry(base) >> 63 & 1) && parked(pfn), "ret=%d", r);
    if (r != 0)
        return;

    int split_ret = 0;
    uint8_t* tail = (uint8_t*)base + TAIL_OFF;
    if (op == OP_MUNMAP)
        split_ret = munmap(tail, PAGE_4K);
    else if (op == OP_MPROTECT)
        split_ret = mprotect(tail, PAGE_4K, PROT_READ);
    else if (op == OP_MREMAP)
        split_ret = mremap(tail, PAGE_4K, PAGE_4K, MREMAP_MAYMOVE | MREMAP_FIXED, dst) == dst ? 0 : -1;
    if (op != OP_NONE)
    {
        snprintf(label, sizeof(label), "%s: the call on the parked tail returns", name);
        check(label, split_ret == 0, "errno=%d", split_ret == 0 ? 0 : errno);
    }

    struct reader rd = {.addr = base};
    pthread_t th;
    pthread_create(&th, NULL, read_thread, &rd);
    double deadline = now() + RESUME_TIMEOUT_S;
    while (atomic_load(&rd.done) == 0 && now() < deadline)
        usleep(1000);
    int resumed = atomic_load(&rd.done);
    // A reader stuck without a page-in is released by hand so the remaining cases can run.
    if (!resumed)
        dbg_write("restore", pfn);
    pthread_join(th, NULL);

    long fetches = status_value("fetches") - fetches0;
    long restored = status_value("restored") - restored0;
    snprintf(label, sizeof(label), "%s: a read of the head resumes through a page-in", name);
    check(label, resumed && rd.value == word_at(op, 0) && fetches >= 1 && restored >= 1,
          "resumed=%d after %.1f ms, fetches Δ=%ld restored Δ=%ld", resumed, (rd.t_end - rd.t_start) * 1e3,
          fetches, restored);

    int intact = verify(base, op, 0, op == OP_NONE || op == OP_MPROTECT ? THP_SIZE : TAIL_OFF);
    if (op == OP_MREMAP)
        intact &= verify((uint64_t*)(dst - TAIL_OFF), op, TAIL_OFF, THP_SIZE);
    snprintf(label, sizeof(label), "%s: data intact and the folio no longer parked", name);
    check(label, intact && !parked(pfn), "intact=%d parked=%d", intact, parked(pfn));

    munmap(dst, PAGE_4K);
    munmap(raw, 2 * THP_SIZE);
}

static int mxim_enable(int on)
{
    int fd = open(MXIM_ENABLE, O_WRONLY);
    if (fd < 0)
        return -errno;
    int ret = write(fd, on ? "1" : "0", 1) == 1 ? 0 : -errno;
    close(fd);
    return ret;
}

// Two disables at once while a folio is parked for the provider must tear it down once.
static void run_concurrent_disable(int node)
{
    uint8_t* raw;
    uint64_t* base;
    uint64_t pfn = make_thp(node, 10, &raw, &base);
    int r = pfn ? dbg_write("quiesce", pfn) : -1;
    check("concurrent disable: a folio is parked for the provider", r == 0, "pfn=%#lx ret=%d", (unsigned long)pfn, r);

    int go[2];
    if (pipe(go) != 0)
        return;
    int ok = 1;
    pid_t kids[2];
    for (int i = 0; i < 2; i++)
    {
        kids[i] = fork();
        if (kids[i] == 0)
        {
            char c;
            if (read(go[0], &c, 1) != 1)
                _exit(2);
            _exit(mxim_enable(0) == 0 ? 0 : 1);
        }
    }
    if (write(go[1], "gg", 2) != 2)
        ok = 0;
    for (int i = 0; i < 2; i++)
    {
        int st = 0;
        waitpid(kids[i], &st, 0);
        ok &= WIFEXITED(st) && WEXITSTATUS(st) == 0;
    }
    check("concurrent disable: both disables return, the provider is gone and the folio restored",
          ok && status_value("ops") == 0 && (pfn == 0 || !parked(pfn)) && verify(base, 10, 0, THP_SIZE),
          "ok=%d ops=%ld parked=%d", ok, status_value("ops"), pfn ? parked(pfn) : -1);
    munmap(raw, 2 * THP_SIZE);
}

// A folio parked with no provider must keep a provider from registering over it.
static void run_register_over_park(int node)
{
    uint8_t* raw;
    uint64_t* base;
    uint64_t pfn = make_thp(node, 11, &raw, &base);
    int r = pfn ? dbg_write("quiesce", pfn) : -1;
    check("register over a park: a folio is parked with no provider", r == 0 && status_value("ops") == 0,
          "pfn=%#lx ret=%d ops=%ld", (unsigned long)pfn, r, status_value("ops"));
    int refused = mxim_enable(1);
    check("register over a park: enable is refused while the folio stays parked", refused == -EBUSY,
          "ret=%d ops=%ld", refused, status_value("ops"));
    if (refused == 0)
        mxim_enable(0);
    int restored = dbg_write("restore", pfn);
    int enabled = mxim_enable(1);
    check("register over a park: enable succeeds once the folio is restored",
          restored == 0 && enabled == 0 && status_value("ops") == 1, "restore=%d enable=%d ops=%ld", restored,
          enabled, status_value("ops"));
    munmap(raw, 2 * THP_SIZE);
}

int main(int argc, char** argv)
{
    int node = argc > 1 ? atoi(argv[1]) : 1;

    printf("node=%d providers=%ld\n", node, status_value("ops"));
    check("a provider is registered", status_value("ops") >= 1, "ops=%ld", status_value("ops"));
    for (int op = OP_NONE; op <= OP_MREMAP; op++)
        run_case(node, op);
    run_concurrent_disable(node);
    run_register_over_park(node);
    printf("\nSUMMARY: %d/%d passed\n", passed, passed + failed);
    return failed ? 1 : 0;
}
