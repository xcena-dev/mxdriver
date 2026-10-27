// E2E helper: owns COUNT THPs on the IM node and answers STATE/READ/FULL/HAMMER/STOP/EXIT on stdin.
// Usage: im_e2e_folio <node> <count> [numa]; "numa" adds MPOL_F_NUMA_BALANCING to the bind,
// so NUMA balancing scans the folios' mappings (a plain MPOL_BIND VMA is skipped).
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define THP_SIZE (2UL << 20)
#define PAGE_4K 4096UL
#define WORDS (THP_SIZE / 8)
#define WORDS_PER_PAGE (PAGE_4K / 8)
#define MAGIC 0x3c3cc3c35a5aa5a5ULL
#define MPOL_BIND 2
#define MPOL_F_NUMA_BALANCING (1 << 13)
#define MAX_FOLIOS 16

static volatile uint64_t* folios[MAX_FOLIOS];
static int count;

static atomic_int hammer_stop;
static int hammer_idx = -1;
static long hammer_passes;
static int hammer_ok = 1;
static double hammer_max_ms;
static pthread_t hammer_thread;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static uint64_t pagemap(volatile void* addr)
{
    uint64_t entry = 0;
    int fd = open("/proc/self/pagemap", O_RDONLY);
    if (pread(fd, &entry, sizeof(entry), ((uintptr_t)addr / PAGE_4K) * sizeof(entry)) != sizeof(entry))
        entry = 0;
    close(fd);
    return entry;
}

static uint64_t expect(int idx, size_t i)
{
    return i ^ (MAGIC + (uint64_t)idx);
}

static int check_sample(int idx)
{
    int ok = 1;
    for (size_t p = 0; p < WORDS; p += WORDS_PER_PAGE)
    {
        ok &= folios[idx][p] == expect(idx, p);
        ok &= folios[idx][p + WORDS_PER_PAGE - 1] == expect(idx, p + WORDS_PER_PAGE - 1);
    }
    return ok;
}

static int check_full(int idx)
{
    int ok = 1;
    for (size_t i = 0; i < WORDS; i++)
        ok &= folios[idx][i] == expect(idx, i);
    return ok;
}

static void* hammer(void* arg)
{
    (void)arg;
    while (!atomic_load(&hammer_stop))
    {
        double t0 = now_ms();
        hammer_ok &= check_sample(hammer_idx);
        double dt = now_ms() - t0;
        if (dt > hammer_max_ms)
            hammer_max_ms = dt;
        hammer_passes++;
    }
    return NULL;
}

static void report_read(int idx, int full)
{
    double t0 = now_ms();
    uint64_t first = folios[idx][0];
    double t_first = now_ms() - t0;
    int ok = first == expect(idx, 0);
    ok &= full ? check_full(idx) : check_sample(idx);
    printf("READ ok=%d first_ms=%.1f total_ms=%.1f\n", ok, t_first, now_ms() - t0);
}

int main(int argc, char** argv)
{
    int node = argc > 1 ? atoi(argv[1]) : 1;
    unsigned long mask = 1UL << node;
    int mode = argc > 3 && strcmp(argv[3], "numa") == 0 ? MPOL_BIND | MPOL_F_NUMA_BALANCING : MPOL_BIND;
    char line[64];

    count = argc > 2 ? atoi(argv[2]) : 1;
    if (count < 1 || count > MAX_FOLIOS)
        return 2;
    for (int f = 0; f < count; f++)
    {
        uint8_t* raw = mmap(NULL, 2 * THP_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        volatile uint64_t* base = (volatile uint64_t*)(((uintptr_t)raw + THP_SIZE - 1) & ~(THP_SIZE - 1));
        if (syscall(SYS_mbind, base, THP_SIZE, mode, &mask, sizeof(mask) * 8, 0) != 0)
            return 3;
        madvise((void*)base, THP_SIZE, MADV_HUGEPAGE);
        for (size_t i = 0; i < WORDS; i++)
            base[i] = expect(f, i);
        folios[f] = base;
        printf("PFN %d %llu\n", f, (unsigned long long)(pagemap(base) & ((1ULL << 55) - 1)));
    }
    fflush(stdout);

    while (fgets(line, sizeof(line), stdin) != NULL)
    {
        int idx = 0;
        sscanf(line + strcspn(line, " "), "%d", &idx);
        if (idx < 0 || idx >= count)
            idx = 0;
        if (strncmp(line, "STATE", 5) == 0)
        {
            uint64_t pm = pagemap(folios[idx]);
            printf("STATE present=%d pfn=%llu\n", (int)(pm >> 63 & 1), (unsigned long long)(pm & ((1ULL << 55) - 1)));
        }
        else if (strncmp(line, "READ", 4) == 0)
        {
            report_read(idx, 0);
        }
        else if (strncmp(line, "FULL", 4) == 0)
        {
            report_read(idx, 1);
        }
        else if (strncmp(line, "HAMMER", 6) == 0)
        {
            hammer_idx = idx;
            atomic_store(&hammer_stop, 0);
            pthread_create(&hammer_thread, NULL, hammer, NULL);
            printf("HAMMER started\n");
        }
        else if (strncmp(line, "STOP", 4) == 0)
        {
            atomic_store(&hammer_stop, 1);
            pthread_join(hammer_thread, NULL);
            printf("STOP passes=%ld ok=%d max_ms=%.1f\n", hammer_passes, hammer_ok, hammer_max_ms);
        }
        else if (strncmp(line, "EXIT", 4) == 0)
        {
            break;
        }
        fflush(stdout);
    }
    return 0;
}
