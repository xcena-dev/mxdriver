// Checks that locked read-modify-write instructions stay atomic across CPUs on a node's memory;
// lost updates mean the memory does not honour atomics (e.g. emulated MMIO).
// Usage (root): atomic_node_test <numa-node> [iterations per thread] [add]; "add" skips the lock.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define THREADS 4
#define MPOL_BIND 2

struct shared
{
    atomic_ulong counter;
    atomic_flag lock;
    unsigned long guarded;
};

static struct shared* sh;
static long iterations;
static int add_only;

static void* worker(void* arg)
{
    (void)arg;
    for (long i = 0; i < iterations; i++)
    {
        atomic_fetch_add(&sh->counter, 1);
        if (add_only)
            continue;
        while (atomic_flag_test_and_set(&sh->lock))
            ;
        sh->guarded++;
        atomic_flag_clear(&sh->lock);
    }
    return NULL;
}

int main(int argc, char** argv)
{
    if (argc < 2)
        return 2;
    int node = atoi(argv[1]);
    unsigned long mask = 1UL << node;
    iterations = argc > 2 ? atol(argv[2]) : 20000;
    add_only = argc > 3;

    void* mem = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED || syscall(SYS_mbind, mem, 4096, MPOL_BIND, &mask, sizeof(mask) * 8, 0) != 0)
        return 3;
    sh = mem;
    sh->counter = 0;
    atomic_flag_clear(&sh->lock);
    sh->guarded = 0;

    int actual = -1;
    syscall(SYS_get_mempolicy, &actual, NULL, 0, mem, 3);

    pthread_t th[THREADS];
    for (int t = 0; t < THREADS; t++)
        pthread_create(&th[t], NULL, worker, NULL);
    for (int t = 0; t < THREADS; t++)
        pthread_join(th[t], NULL);

    unsigned long want = (unsigned long)THREADS * iterations;
    if (add_only)
        sh->guarded = want;
    unsigned long got = atomic_load(&sh->counter);
    printf("node=%d page_node=%d expected=%lu atomic_add=%lu lock_guarded=%lu %s\n", node, actual, want, got,
           sh->guarded, got == want && sh->guarded == want ? "ATOMIC_OK" : "ATOMIC_BROKEN");
    return got == want && sh->guarded == want ? 0 : 1;
}
