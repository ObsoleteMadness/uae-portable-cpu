#ifndef PORTABLE_DIRENT_H
#define PORTABLE_DIRENT_H

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <sys/stat.h>

#ifndef S_IFMT
#define S_IFMT _S_IFMT
#endif
#ifndef S_IFDIR
#define S_IFDIR _S_IFDIR
#endif
#ifndef S_IFREG
#define S_IFREG _S_IFREG
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif

struct dirent {
    char d_name[MAX_PATH];
};

typedef struct {
    HANDLE handle;
    WIN32_FIND_DATAA data;
    struct dirent ent;
    int first;
} DIR;

static inline DIR *opendir(const char *path) {
    char search_path[MAX_PATH];
    snprintf(search_path, sizeof(search_path), "%s\\*", path);
    DIR *dir = (DIR *)malloc(sizeof(DIR));
    if (!dir) return NULL;
    dir->first = 1;
    dir->handle = FindFirstFileA(search_path, &dir->data);
    if (dir->handle == INVALID_HANDLE_VALUE) {
        free(dir);
        return NULL;
    }
    return dir;
}

static inline struct dirent *readdir(DIR *dir) {
    if (!dir || dir->handle == INVALID_HANDLE_VALUE) return NULL;
    if (dir->first) {
        dir->first = 0;
    } else {
        if (!FindNextFileA(dir->handle, &dir->data)) {
            return NULL;
        }
    }
    strncpy(dir->ent.d_name, dir->data.cFileName, MAX_PATH - 1);
    dir->ent.d_name[MAX_PATH - 1] = '\0';
    return &dir->ent;
}

static inline int closedir(DIR *dir) {
    if (!dir) return -1;
    if (dir->handle != INVALID_HANDLE_VALUE) {
        FindClose(dir->handle);
    }
    free(dir);
    return 0;
}

#else
#include <dirent.h>
#endif

#endif /* PORTABLE_DIRENT_H */
