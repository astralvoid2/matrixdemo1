#ifndef DEMO_PLATFORM_H
#define DEMO_PLATFORM_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define DEMO_PATH_CAPACITY 4096
/* Paths are UTF-8, including on Windows. Return 0 on success. */
int platform_data_path(char *path, size_t size, const char *name, int make_dir);
FILE *platform_fopen(const char *path, const char *mode);
int platform_remove_file(const char *path);
void platform_read_memory(long *rss, long *anon, long *file, long *shmem);
uint64_t platform_executable_size(void);
const char *platform_basename(const char *path);
#endif
