// With ccma holding system-ram chunks on the IM node, IM leaves them alone and still parks THPs.
// Needs ccma armed on the node, a registered provider (IM enabled on the device) and the test hooks.
// Usage (root): im_ccma_test <numa-node> <start-pfn> <nr-pages>
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
#define THP_PAGES 512UL
#define PAGE_4K 4096UL
#define DBG "/sys/kernel/debug/im_soft_fault/"
#define CCMA "/sys/devices/virtual/misc/ccma/"
#define MAGIC 0x1e1ee1e1c3a5a53cULL
#define MPOL_BIND 2
#define KPF_COMPOUND_HEAD 15
#define KPF_THP 22
#define KPF_OFFLINE 23
#define SAMPLES 8
#define ARM_TIMEOUT_S 60.0

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

static uint64_t read_u64_at(const char* path, uint64_t index)
{
    int fd = open(path, O_RDONLY);
    uint64_t value = 0;
    if (pread(fd, &value, sizeof(value), index * sizeof(value)) != sizeof(value))
        value = 0;
    close(fd);
    return value;
}

static uint64_t kpageflags(uint64_t pfn)
{
    return read_u64_at("/proc/kpageflags", pfn);
}

static uint64_t pagemap_entry(void* addr)
{
    return read_u64_at("/proc/self/pagemap", (uintptr_t)addr / PAGE_4K);
}

static int write_str(const char* path, const char* text)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -errno;
    int len = strlen(text);
    int ret = write(fd, text, len) == len ? 0 : -errno;
    close(fd);
    return ret;
}

static long read_long(const char* path)
{
    char buf[64] = "";
    int fd = open(path, O_RDONLY);
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    buf[n > 0 ? n : 0] = '\0';
    return n > 0 ? atol(buf) : -1;
}

static int dbg_write(const char* file, uint64_t pfn)
{
    char path[128], buf[32];
    snprintf(path, sizeof(path), DBG "%s", file);
    snprintf(buf, sizeof(buf), "%#lx", (unsigned long)pfn);
    return write_str(path, buf);
}

static int parked(uint64_t pfn)
{
    char buf[8192], key[64];
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

static int verify(volatile uint64_t* base, int idx)
{
    for (size_t off = 0; off < THP_SIZE; off += 8)
        if (base[off / 8] != word_at(idx, off))
            return 0;
    return 1;
}

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
    int is_thp = (kf >> KPF_THP & 1) && (kf >> KPF_COMPOUND_HEAD & 1) && (pfn % THP_PAGES == 0);
    return is_thp ? pfn : 0;
}

struct reader
{
    volatile uint64_t* addr;
    uint64_t value;
    atomic_int done;
};

static void* read_thread(void* arg)
{
    struct reader* r = arg;
    r->value = *r->addr;
    atomic_store(&r->done, 1);
    return NULL;
}

// Reads the head of a parked THP and waits for the page-in; releases it by hand if none comes.
static int read_resumes(uint64_t* base, uint64_t pfn, int idx)
{
    struct reader rd = {.addr = base};
    pthread_t th;
    pthread_create(&th, NULL, read_thread, &rd);
    double deadline = now() + 30.0;
    while (atomic_load(&rd.done) == 0 && now() < deadline)
        usleep(1000);
    int resumed = atomic_load(&rd.done);
    if (!resumed)
        dbg_write("restore", pfn);
    pthread_join(th, NULL);
    return resumed && rd.value == word_at(idx, 0);
}

// IM must refuse to park ccma's chunks and leave them exactly as ccma holds them.
static void run_chunks_untouched(uint64_t start, uint64_t nr)
{
    uint64_t found[SAMPLES];
    int n = 0;
    for (uint64_t pfn = start; pfn < start + nr && n < SAMPLES; pfn += 64 * THP_PAGES)
        if (kpageflags(pfn) >> KPF_OFFLINE & 1)
            found[n++] = pfn;
    check("ccma holds offline chunk pages in the IM range", n > 0, "sampled %d", n);

    int refused = 1, intact = 1;
    for (int i = 0; i < n; i++)
    {
        int r = dbg_write("quiesce", found[i]);
        refused &= r == -EINVAL && !parked(found[i]);
        intact &= kpageflags(found[i]) >> KPF_OFFLINE & 1;
    }
    check("a park attempt on a ccma chunk is refused", n > 0 && refused, "samples=%d", n);
    check("the chunk stays offline, still held by ccma", n > 0 && intact, "samples=%d", n);
}

// Next to ccma's chunks, an anonymous THP still parks and pages back in through the provider.
static void run_park_beside_chunks(int node)
{
    uint8_t* raw;
    uint64_t* base;
    uint64_t pfn = make_thp(node, 1, &raw, &base);
    int r = pfn ? dbg_write("quiesce", pfn) : -1;
    check("an anonymous THP beside the chunks parks", r == 0 && parked(pfn), "pfn=%#lx ret=%d", (unsigned long)pfn, r);
    if (r == 0)
        check("its read resumes through a page-in with intact data", read_resumes(base, pfn, 1) && verify(base, 1),
              "parked=%d", parked(pfn));
    munmap(raw, 2 * THP_SIZE);
}

// ccma arming while a folio is parked must not wait on the park: the parked window is skipped,
// and the folio stays where it is. An arm stuck behind the park is released by restoring the folio.
static void run_arm_over_park(int node)
{
    uint8_t* raw;
    uint64_t* base;
    write_str(CCMA "reserve_pct", "0");
    uint64_t pfn = make_thp(node, 2, &raw, &base);
    int r = pfn ? dbg_write("quiesce", pfn) : -1;
    check("arm over a park: a THP is parked before ccma arms", r == 0, "pfn=%#lx ret=%d", (unsigned long)pfn, r);
    if (r != 0)
        return;

    long fw0 = read_long(CCMA "failed_windows");
    double t0 = now();
    pid_t kid = fork();
    if (kid == 0)
        _exit(write_str(CCMA "reserve_pct", "100") == 0 ? 0 : 1);
    int st = 0, done = 0;
    while (!done && now() < t0 + ARM_TIMEOUT_S)
    {
        done = waitpid(kid, &st, WNOHANG) == kid;
        if (!done)
            usleep(10000);
    }
    double secs = now() - t0;
    int still_parked = parked(pfn) && !(kpageflags(pfn) >> KPF_OFFLINE & 1);
    if (!done)
    {
        dbg_write("restore", pfn);
        waitpid(kid, &st, 0);
    }
    long fw1 = read_long(CCMA "failed_windows");
    long chunks = read_long(CCMA "total_chunks");
    check("arm over a park: ccma arms without waiting on the park and skips its window",
          done && WIFEXITED(st) && WEXITSTATUS(st) == 0 && fw1 > fw0 && chunks > 0,
          "finished=%d in %.1fs, failed_windows Δ=%ld total_chunks=%ld", done, secs, fw1 - fw0, chunks);
    check("arm over a park: the folio is still parked where it was, not taken by ccma", still_parked, NULL);
    int intact = parked(pfn) ? read_resumes(base, pfn, 2) && verify(base, 2) : verify(base, 2);
    check("arm over a park: the folio comes back with intact data", intact, NULL);
    munmap(raw, 2 * THP_SIZE);
    write_str(CCMA "reserve_pct", "0");
}

int main(int argc, char** argv)
{
    if (argc < 4)
        return 2;
    int node = atoi(argv[1]);
    uint64_t start = strtoull(argv[2], NULL, 0);
    uint64_t nr = strtoull(argv[3], NULL, 0);

    printf("node=%d range=%#lx+%#lx ccma total_chunks=%ld reserve_pct=%ld\n", node, (unsigned long)start,
           (unsigned long)nr, read_long(CCMA "total_chunks"), read_long(CCMA "reserve_pct"));
    run_chunks_untouched(start, nr);
    run_park_beside_chunks(node);
    run_arm_over_park(node);
    printf("\nSUMMARY: %d/%d passed\n", passed, passed + failed);
    return failed ? 1 : 0;
}
