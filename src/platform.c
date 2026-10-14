/* Small operating-system boundary: config paths, UTF-8 files and HUD stats. */
#define _GNU_SOURCE
#include "platform.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <psapi.h>

static wchar_t *wide_string(const char *text)
{
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (!count) return NULL;
    wchar_t *result = malloc((size_t)count * sizeof *result);
    if (result && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, result, count)) {
        free(result);
        return NULL;
    }
    return result;
}

int platform_data_path(char *path, size_t size, const char *name, int make_dir)
{
    PWSTR base = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, make_dir ? KF_FLAG_CREATE : 0, NULL, &base)))
        return -1;
    size_t count = wcslen(base) + 32;
    wchar_t *dir = calloc(count, sizeof *dir);
    int result = -1;
    if (dir) {
        swprintf(dir, count, L"%ls\\MatrixRain", base);
        if (!make_dir || CreateDirectoryW(dir, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) {
            int bytes = WideCharToMultiByte(CP_UTF8, 0, dir, -1, NULL, 0, NULL, NULL);
            if (bytes > 0 && (size_t)bytes < size &&
                WideCharToMultiByte(CP_UTF8, 0, dir, -1, path, bytes, NULL, NULL)) {
                int written = snprintf(path + bytes - 1, size - (size_t)bytes + 1, "/%s", name);
                if (written >= 0 && (size_t)written < size - (size_t)bytes + 1) result = 0;
            }
        }
        free(dir);
    }
    CoTaskMemFree(base);
    return result;
}

FILE *platform_fopen(const char *path, const char *mode)
{
    wchar_t *wide_path = wide_string(path), *wide_mode = wide_string(mode);
    FILE *result = wide_path && wide_mode ? _wfopen(wide_path, wide_mode) : NULL;
    free(wide_path);
    free(wide_mode);
    return result;
}

int platform_remove_file(const char *path)
{
    wchar_t *wide_path = wide_string(path);
    int result = wide_path ? _wremove(wide_path) : -1;
    free(wide_path);
    return result;
}

void platform_read_memory(long *rss, long *anon, long *file, long *shmem)
{
    *rss = *anon = *file = *shmem = 0;
    PROCESS_MEMORY_COUNTERS_EX counters = {0};
    counters.cb = sizeof counters;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&counters, sizeof counters)) {
        *rss = (long)(counters.WorkingSetSize / 1024);
        *anon = (long)(counters.PrivateUsage / 1024); /* private commit, not resident anonymous memory */
    }
}

uint64_t platform_executable_size(void)
{
    wchar_t path[32768];
    DWORD length = GetModuleFileNameW(NULL, path, (DWORD)(sizeof path / sizeof *path));
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    if (!length || length >= sizeof path / sizeof *path ||
        !GetFileAttributesExW(path, GetFileExInfoStandard, &attributes)) return 0;
    return ((uint64_t)attributes.nFileSizeHigh << 32) | attributes.nFileSizeLow;
}
#else
#include <sys/stat.h>
#include <unistd.h>

int platform_data_path(char *path, size_t size, const char *name, int make_dir)
{
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char dir[DEMO_PATH_CAPACITY];
    int written;
    if (xdg && *xdg) written = snprintf(dir, sizeof dir, "%s/matrixrain", xdg);
    else if (home) written = snprintf(dir, sizeof dir, "%s/.config/matrixrain", home);
    else return -1;
    if (written < 0 || (size_t)written >= sizeof dir) return -1;
    if (make_dir) {
        char parent[DEMO_PATH_CAPACITY];
        snprintf(parent, sizeof parent, "%s", dir);
        char *slash = strrchr(parent, '/');
        if (slash) { *slash = '\0'; mkdir(parent, 0755); }
        mkdir(dir, 0755);
    }
    written = snprintf(path, size, "%s/%s", dir, name);
    return written >= 0 && (size_t)written < size ? 0 : -1;
}

FILE *platform_fopen(const char *path, const char *mode) { return fopen(path, mode); }
int platform_remove_file(const char *path) { return remove(path); }

void platform_read_memory(long *rss, long *anon, long *file, long *shmem)
{
    *rss = *anon = *file = *shmem = 0;
    FILE *fp = fopen("/proc/self/status", "r");
    if (!fp) return;
    char line[256];
    while (fgets(line, sizeof line, fp)) {
        sscanf(line, "VmRSS: %ld", rss);
        sscanf(line, "RssAnon: %ld", anon);
        sscanf(line, "RssFile: %ld", file);
        sscanf(line, "RssShmem: %ld", shmem);
    }
    fclose(fp);
}

uint64_t platform_executable_size(void)
{
    struct stat info;
    return stat("/proc/self/exe", &info) == 0 ? (uint64_t)info.st_size : 0;
}
#endif

const char *platform_basename(const char *path)
{
    const char *result = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') result = p + 1;
    return result;
}
