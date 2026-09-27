#include <jemalloc/jemalloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

static void control(const char *name, void *output, size_t output_size,
    void *input, size_t input_size) {
    size_t actual_size = output_size;
    int error = mallctl(name, output, output ? &actual_size : NULL,
        input, input_size);
    if (error != 0 || (output && actual_size != output_size)) {
        fprintf(stderr, "%s: %s\n", name,
            error ? strerror(error) : "unexpected value size");
        exit(EXIT_FAILURE);
    }
}

static size_t read_size(const char *name) {
    size_t value;
    control(name, &value, sizeof(value), NULL, 0);
    return value;
}

static size_t show(const char *phase, unsigned arena_index, unsigned bin_index) {
    uint64_t epoch = 1;
    char name[128];
    control("epoch", NULL, 0, &epoch, sizeof(epoch));
    snprintf(name, sizeof(name), "stats.arenas.%u.bins.%u.curregs",
        arena_index, bin_index);
    size_t regions = read_size(name);
    snprintf(name, sizeof(name), "stats.arenas.%u.pactive", arena_index);
    size_t active = read_size(name);
    snprintf(name, sizeof(name), "stats.arenas.%u.pdirty", arena_index);
    size_t dirty = read_size(name);
    printf("%-10s curregs=%zu pactive=%zu pdirty=%zu\n",
        phase, regions, active, dirty);
    return regions;
}

int main(void) {
    const char *version;
    unsigned bin_count;
    char name[128];
    control("version", &version, sizeof(version), NULL, 0);
    control("arenas.nbins", &bin_count, sizeof(bin_count), NULL, 0);
    size_t usable = nallocx(100, 0);
    if (usable == 0) {
        return EXIT_FAILURE;
    }
    unsigned bin_index = bin_count;
    for (unsigned candidate = 0; candidate < bin_count; ++candidate) {
        snprintf(name, sizeof(name), "arenas.bin.%u.size", candidate);
        if (read_size(name) == usable) {
            bin_index = candidate;
            break;
        }
    }
    if (bin_index == bin_count) {
        fprintf(stderr, "The requested size is not a small size class.\n");
        return EXIT_FAILURE;
    }
    unsigned regions_per_slab;
    snprintf(name, sizeof(name), "arenas.bin.%u.nregs", bin_index);
    control(name, &regions_per_slab, sizeof(regions_per_slab), NULL, 0);
    snprintf(name, sizeof(name), "arenas.bin.%u.slab_size", bin_index);
    size_t slab_size = read_size(name);
    snprintf(name, sizeof(name), "arenas.bin.%u.size", bin_count - 1);
    size_t small_max = read_size(name);
    size_t tcache_max = read_size("opt.tcache_max");
    size_t page_size = read_size("arenas.page");
    bool hpa;
    control("opt.hpa", &hpa, sizeof(hpa), NULL, 0);
    if (hpa) {
        fprintf(stderr, "Run this experiment with hpa:false.\n");
        return EXIT_FAILURE;
    }
    unsigned arena_index;
    unsigned tcache_index;
    control("arenas.create", &arena_index, sizeof(arena_index), NULL, 0);
    control("tcache.create", &tcache_index, sizeof(tcache_index), NULL, 0);
    ssize_t disabled_decay = -1;
    snprintf(name, sizeof(name), "arena.%u.dirty_decay_ms", arena_index);
    control(name, NULL, 0, &disabled_decay, sizeof(disabled_decay));
    snprintf(name, sizeof(name), "arena.%u.muzzy_decay_ms", arena_index);
    control(name, NULL, 0, &disabled_decay, sizeof(disabled_decay));
    printf("version=%s\n", version);
    printf("page=%zu small_max=%zu tcache_max=%zu\n",
        page_size, small_max, tcache_max);
    printf("request=100 usable=%zu bin=%u nregs=%u slab_size=%zu\n",
        usable, bin_index, regions_per_slab, slab_size);
    void *object = mallocx(100,
        MALLOCX_ARENA(arena_index) | MALLOCX_TCACHE(tcache_index));
    if (object == NULL) {
        return EXIT_FAILURE;
    }
    memset(object, 0x5a, 100);
    size_t allocated_regions = show("allocated", arena_index, bin_index);
    dallocx(object, MALLOCX_TCACHE(tcache_index));
    size_t cached_regions = show("freed", arena_index, bin_index);
    control("tcache.flush", NULL, 0, &tcache_index, sizeof(tcache_index));
    size_t flushed_regions = show("flushed", arena_index, bin_index);
    snprintf(name, sizeof(name), "arena.%u.purge", arena_index);
    control(name, NULL, 0, NULL, 0);
    size_t purged_regions = show("purged", arena_index, bin_index);
    control("tcache.destroy", NULL, 0, &tcache_index, sizeof(tcache_index));
    snprintf(name, sizeof(name), "arena.%u.destroy", arena_index);
    control(name, NULL, 0, NULL, 0);
    if (allocated_regions == 0 || cached_regions == 0
        || flushed_regions != 0 || purged_regions != 0) {
        fprintf(stderr, "Unexpected region accounting for this experiment.\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
