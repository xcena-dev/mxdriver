// Writes back and invalidates [offset, offset+size) of a devdax device in the CPU caches.
// Usage: im_clflush <devdax path> <offset> <size>; replaces /dev/xcena_cache on real hardware,
// where CLFLUSH through any mapping of the same physical lines reaches the device.
#define _GNU_SOURCE
#include <fcntl.h>
#include <immintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define DAX_ALIGN (2UL << 20)
#define LINE 64UL

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        fprintf(stderr, "usage: %s <devdax path> <offset> <size>\n", argv[0]);
        return 2;
    }
    uint64_t offset = strtoull(argv[2], NULL, 0);
    uint64_t size = strtoull(argv[3], NULL, 0);
    uint64_t start = offset & ~(DAX_ALIGN - 1);
    uint64_t len = ((offset + size + DAX_ALIGN - 1) & ~(DAX_ALIGN - 1)) - start;

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0)
    {
        perror("open");
        return 1;
    }
    char* map = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, start);
    if (map == MAP_FAILED)
    {
        perror("mmap");
        return 1;
    }
    for (char* p = map + (offset - start); p < map + (offset - start) + size; p += LINE)
        _mm_clflush(p);
    _mm_mfence();
    munmap(map, len);
    close(fd);
    return 0;
}
