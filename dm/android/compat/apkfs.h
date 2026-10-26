// Game files inside the installed APKs (apkfs.c): the file calls the host makes, routed through wrappers that resolve paths under /nxapk/ to APK entries and pass every
// other path / descriptor to the real call. Included at the end of darwin.h (every Android host file); apkfs.c itself sees the real names
#pragma once
#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>
int apkfs_is(const char *path);
int dm_open(const char *path, int flags, ...);
int dm_close(int fd);
ssize_t dm_read(int fd, void *buf, size_t n);
ssize_t dm_pread(int fd, void *buf, size_t n, off_t off);
off_t dm_lseek(int fd, off_t off, int whence);
int dm_fstat(int fd, struct stat *st);
int dm_stat(const char *path, struct stat *st);
int dm_lstat(const char *path, struct stat *st);
int dm_access(const char *path, int mode);
ssize_t dm_lgetxattr(const char *path, const char *name, void *v, size_t n);
DIR *dm_opendir(const char *path);
struct dirent *dm_readdir(DIR *d);
int dm_closedir(DIR *d);
int dm_dirfd(DIR *d);
int dm_fstatat(int dfd, const char *name, struct stat *st, int flags);
FILE *dm_fopen(const char *path, const char *mode);
void *dm_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
#ifndef APKFS_IMPL
#define open(...) dm_open(__VA_ARGS__)
#define close(fd) dm_close(fd)
#define read(fd, b, n) dm_read(fd, b, n)
#define pread(fd, b, n, o) dm_pread(fd, b, n, o)
#define lseek(fd, o, w) dm_lseek(fd, o, w)
#define fstat(fd, st) dm_fstat(fd, st)
#define stat(p, st) dm_stat(p, st)
#define lstat(p, st) dm_lstat(p, st)
#define access(p, m) dm_access(p, m)
#define lgetxattr(p, n, v, s) dm_lgetxattr(p, n, v, s)
#define opendir(p) dm_opendir(p)
#define readdir(d) dm_readdir(d)
#define closedir(d) dm_closedir(d)
#define dirfd(d) dm_dirfd(d)
#define fstatat(d, n, s, f) dm_fstatat(d, n, s, f)
#define fopen(p, m) dm_fopen(p, m)
#define mmap(a, l, p, f, fd, o) dm_mmap(a, l, p, f, fd, o)
#endif
