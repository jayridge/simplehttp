/*
 * region.c
 *
 * Region based allocator sized in multiples of pagesize.
 * These routines are specific to the operating characteristics
 * of simplequeue.
 *
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
#include "simplehttp/queue.h"

struct region {
    TAILQ_ENTRY(region) entries;
    uint32_t ref_count;
    void *write_head;
    char data[1];
};
TAILQ_HEAD(region_list, region) regions;

static long page_size;
static long region_size;
static int nregions = 0;

#define ALIGN_BYTES 8
#define PAD(o,a) ((-o) & (a - 1))

static struct region *new_region()
{
    struct region *reg;
    void *start_addr;

    /*
     * Mapped memory is not always reclaimed on centos. I am setting the
     * start address of the mapped region in an attempt to avoid interleaving
     * with heap allocations - this is probably total bullshit.
     *
     */
    start_addr = (void *)0xa00000;
    start_addr += region_size*nregions;
    reg = (struct region *)mmap(start_addr, region_size, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
    if (reg == MAP_FAILED) {
        fprintf(stderr, "mmap() failed: %s\n", strerror(errno));
        return NULL;
    }
    fprintf(stderr, "mmap() %p %ld\n", reg, region_size);
    posix_madvise(reg, region_size, POSIX_MADV_SEQUENTIAL);
    reg->ref_count = 0;
    reg->write_head = reg->data + PAD(sizeof(struct region), ALIGN_BYTES);
    TAILQ_INSERT_TAIL(&regions, reg, entries);
    nregions++;
    return reg;
}

void region_init(size_t pages_per_region)
{
    page_size = sysconf(_SC_PAGESIZE);
    region_size = page_size * pages_per_region;
    TAILQ_INIT(&regions);
}

void *region_alloc(size_t size)
{
    void *ptr;
    struct region *reg = TAILQ_LAST(&regions, region_list);
    size_t aligned_size = size + PAD(size, ALIGN_BYTES);

    /*
     * Requests larger than 1/64th of the region are malloc'd
     * directly and padded to page_size.
     *
     */
    if (aligned_size > (region_size >> 6)) {
        return malloc(size + PAD(size, page_size));
    }
    if (!reg || (region_size - (reg->write_head - (void *)reg)) < aligned_size) {
        reg = new_region();
    }
    ptr = reg->write_head;
    reg->ref_count += 1;
    reg->write_head += aligned_size;
    return ptr;
}

void region_free(void *ptr)
{
    struct region *reg;

    reg = TAILQ_FIRST(&regions);
    while (reg) {
        if (ptr > (void *)reg && ptr < ((void *)reg + region_size)) {
            if (--reg->ref_count == 0) {
                TAILQ_REMOVE(&regions, reg, entries);
                fprintf(stderr, "munmap() %p\n", reg);
                if (munmap((void *)reg, region_size) != 0) {
                    fprintf(stderr, "munmap() failed: %s\n", strerror(errno));
                }
                nregions--;
            }
            return;
        }
        reg = TAILQ_NEXT(reg, entries);
    }
    /*
     * This item is not within a region. Assume this was allocated via malloc.
     *
     */
    free(ptr);
}
