// Parks a PMD-mapped anonymous THP behind migration entries and restores it.
// Usage (root): im_mm_test <numa-node>
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
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
#define DBG "/sys/kernel/debug/im_soft_fault/"
#define MAGIC 0x5a5aa5a5c3c33c3cULL
#define MPOL_BIND 2
#define MPOL_F_NODE (1 << 0)
#define MPOL_F_ADDR (1 << 1)
#define KPF_COMPOUND_HEAD 15
#define KPF_THP 22

static int passed, failed;

static void check(const char *name, int ok, const char *fmt, ...)
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
    pread(fd, &entry, sizeof(entry), ((uintptr_t)addr / PAGE_4K) * sizeof(entry));
    close(fd);
    return entry;
}

static uint64_t kpageflags(uint64_t pfn)
{
    int fd = open("/proc/kpageflags", O_RDONLY);
    uint64_t flags = 0;
    pread(fd, &flags, sizeof(flags), pfn * sizeof(flags));
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

static void dbg_status(char* out, size_t size)
{
    int fd = open(DBG "status", O_RDONLY);
    ssize_t n = read(fd, out, size - 1);
    out[n > 0 ? n : 0] = '\0';
    close(fd);
}

static int status_faults(uint64_t pfn)
{
    char buf[4096], key[64];
    dbg_status(buf, sizeof(buf));
    snprintf(key, sizeof(key), "pfn=%#lx faults=", (unsigned long)pfn);
    char* p = strstr(buf, key);
    return p ? atoi(p + strlen(key)) : -1;
}

static uint64_t word_at(size_t off)
{
    return (off / 8) ^ MAGIC;
}

static int verify(volatile uint64_t* base, size_t stride)
{
    for (size_t off = 0; off < THP_SIZE; off += stride)
        if (base[off / 8] != word_at(off))
            return 0;
    return 1;
}

struct accessor
{
    volatile uint64_t* addr;
    int write;
    uint64_t value;
    atomic_int done;
    double t_start, t_end;
};

static void* access_thread(void* arg)
{
    struct accessor* a = arg;
    a->t_start = now();
    if (a->write)
        *a->addr = a->value;
    else
        a->value = *a->addr;
    a->t_end = now();
    atomic_store(&a->done, 1);
    return NULL;
}

static atomic_int spin_stop;
static atomic_ulong spin_count;

static void* spin_thread(void* arg)
{
    (void)arg;
    while (atomic_load(&spin_stop) == 0)
        atomic_fetch_add(&spin_count, 1);
    return NULL;
}

int main(int argc, char** argv)
{
    int node = argc > 1 ? atoi(argv[1]) : 0;
    char status[4096];

    uint8_t* raw = mmap(NULL, 2 * THP_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint64_t* base = (uint64_t*)(((uintptr_t)raw + THP_SIZE - 1) & ~(THP_SIZE - 1));
    unsigned long mask = 1UL << node;
    long mb = syscall(SYS_mbind, base, THP_SIZE, MPOL_BIND, &mask, sizeof(mask) * 8, 0);
    madvise(base, THP_SIZE, MADV_HUGEPAGE);

    double t0 = now();
    for (size_t off = 0; off < THP_SIZE; off += 8)
        base[off / 8] = word_at(off);
    printf("node=%d mbind=%ld filled 2 MiB in %.1fs\n", node, mb, now() - t0);

    uint64_t pm = pagemap_entry(base);
    uint64_t pfn = pm & ((1ULL << 55) - 1);
    int actual_node = -1;
    syscall(SYS_get_mempolicy, &actual_node, NULL, 0, base, MPOL_F_NODE | MPOL_F_ADDR);
    uint64_t kf = kpageflags(pfn);
    int is_thp = (kf >> KPF_THP & 1) && (kf >> KPF_COMPOUND_HEAD & 1) && (pfn % 512 == 0);
    check("region is a PMD-mapped THP on the requested node", is_thp && actual_node == node,
          "pfn=%#lx node=%d kpageflags=%#lx", (unsigned long)pfn, actual_node, (unsigned long)kf);
    if (!is_thp)
        return 1;

    // Rejections.
    int r_small = dbg_write("quiesce", pfn + 1);
    check("quiesce of a non-head pfn is refused", r_small == -EINVAL, "ret=%d", r_small);
    int r_unknown = dbg_write("restore", pfn);
    check("restore of an unparked pfn is refused", r_unknown == -ENOENT, "ret=%d", r_unknown);

    // 1. Park; the PMD turns into a non-present migration entry.
    int r = dbg_write("quiesce", pfn);
    pm = pagemap_entry(base);
    check("quiesce succeeds and unmaps the PMD", r == 0 && !(pm >> 63 & 1) && (pm >> 62 & 1),
          "ret=%d present=%d swap=%d", r, (int)(pm >> 63 & 1), (int)(pm >> 62 & 1));
    int r_twice = dbg_write("quiesce", pfn);
    check("second quiesce of the same folio is refused", r_twice == -EBUSY, "ret=%d", r_twice);

    // 2. A reader sleeps; another thread keeps running; mmap_lock stays available.
    struct accessor rd = {.addr = &base[(THP_SIZE / 2) / 8], .write = 0};
    pthread_t t_rd, t_spin;
    atomic_store(&spin_stop, 0);
    pthread_create(&t_spin, NULL, spin_thread, NULL);
    pthread_create(&t_rd, NULL, access_thread, &rd);
    usleep(300 * 1000);
    unsigned long spin0 = atomic_load(&spin_count);
    double tm = now();
    void* other = mmap(NULL, PAGE_4K, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    munmap(other, PAGE_4K);
    double mmap_ms = (now() - tm) * 1e3;
    usleep(1200 * 1000);
    unsigned long spin1 = atomic_load(&spin_count);
    int faults = status_faults(pfn);
    check("reader blocks while the folio is parked", atomic_load(&rd.done) == 0, "faults=%d", faults);
    check("fault reached the soft-fault hook", faults >= 1, "faults=%d", faults);
    check("other threads keep running", spin1 > spin0, "spin Δ=%lu", spin1 - spin0);
    check("mmap/munmap is not blocked by the sleeping fault", mmap_ms < 200.0, "%.1f ms", mmap_ms);

    // 3. Restore; the reader resumes on the same folio with the original bytes.
    double tr = now();
    r = dbg_write("restore", pfn);
    pthread_join(t_rd, NULL);
    pm = pagemap_entry(base);
    uint64_t pfn_after = pm & ((1ULL << 55) - 1);
    check("restore succeeds and the reader resumes", r == 0 && atomic_load(&rd.done) == 1,
          "ret=%d waited=%.2fs resumed %.1f ms after restore", r, rd.t_end - rd.t_start,
          (rd.t_end - tr) * 1e3);
    check("reader sees the original value", rd.value == word_at(THP_SIZE / 2), NULL);
    check("same pfn, still a THP after restore",
          (pm >> 63 & 1) && pfn_after == pfn && (kpageflags(pfn) >> KPF_THP & 1), "pfn=%#lx",
          (unsigned long)pfn_after);

    // 4. A writer also sleeps and its store lands after restore.
    r = dbg_write("quiesce", pfn);
    struct accessor wr = {.addr = &base[8], .write = 1, .value = 0x1122334455667788ULL};
    pthread_create(&t_rd, NULL, access_thread, &wr);
    usleep(800 * 1000);
    int write_blocked = atomic_load(&wr.done) == 0;
    dbg_write("restore", pfn);
    pthread_join(t_rd, NULL);
    check("writer blocks while parked and lands after restore",
          r == 0 && write_blocked && base[8] == 0x1122334455667788ULL, "blocked=%d", write_blocked);
    base[8] = word_at(64);

    // 5. Two mappings (after fork) are parked together; a killed waiter exits promptly.
    pid_t child = fork();
    if (child == 0)
    {
        usleep(500 * 1000);
        volatile uint64_t v = base[100];
        (void)v;
        _exit(0);
    }
    r = dbg_write("quiesce", pfn);
    pm = pagemap_entry(base);
    check("folio mapped by parent and child is parked", r == 0 && !(pm >> 63 & 1), "ret=%d", r);
    usleep(1200 * 1000);
    int child_faults = status_faults(pfn);
    int st = 0;
    pid_t w = waitpid(child, &st, WNOHANG);
    double tk = now();
    kill(child, SIGKILL);
    waitpid(child, &st, 0);
    double kill_ms = (now() - tk) * 1e3;
    check("child blocks on the parked folio", w == 0 && child_faults >= 1, "faults=%d", child_faults);
    check("SIGKILL ends the wait promptly", WIFSIGNALED(st) && kill_ms < 500.0, "%.1f ms", kill_ms);
    r = dbg_write("restore", pfn);
    check("restore after the waiter died", r == 0, "ret=%d", r);

    // 6. Whole range intact, nothing left parked.
    atomic_store(&spin_stop, 1);
    pthread_join(t_spin, NULL);
    check("all 2 MiB intact after three park/restore cycles", verify(base, 8), NULL);
    dbg_status(status, sizeof(status));
    check("no folio left parked", strstr(status, "\npfn=") == NULL, "%.*s", (int)strcspn(status, "\n"), status);

    printf("\nSUMMARY: %d/%d passed\n", passed, passed + failed);
    munmap(raw, 2 * THP_SIZE);
    return failed ? 1 : 0;
}
