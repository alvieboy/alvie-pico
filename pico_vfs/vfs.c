#include "pico/vfs.h"
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>

#ifdef __NEWLIB__
#include <sys/reent.h>
#else
#define __errno_r(reent) errno
#endif

#include <stdio.h>
#include <pico/sync.h>
#include <pico/platform.h>

#define MAX_FDS 16
#define VFS_MAX_COUNT 4
#define LEN_PATH_PREFIX_IGNORED SIZE_MAX /* special length value for VFS which is never recognised by open() */

typedef struct pico_vfs_fd_table_
{
    int8_t vfs_index;
    vfs_fd_t local_fd;
    bool permanent;
} pico_vfs_fd_table_t;

typedef struct pico_vfs_entry_ {
    pico_vfs_ops_t ops;

    char path_prefix[PICO_VFS_BASE_PATH_MAX]; // path prefix mapped to this VFS

    size_t path_prefix_len; // micro-optimization to avoid doing extra strlen
    uint8_t index;             // index of this structure in s_vfs array
    void *drvctx; // driver context
} pico_vfs_entry_t;

typedef struct pico_vfs_internal_dir_
{
    struct __dirstream stream;
    struct dirent dir;
    int8_t iter;
} pico_vfs_internal_dir_t;

#define FD_TABLE_ENTRY_UNUSED   (pico_vfs_fd_table_t) { .vfs_index = -1, .local_fd = { .fd=-1 }, .permanent = false }

static pico_vfs_fd_table_t s_fd_table[MAX_FDS];
static pico_vfs_entry_t* s_vfs[VFS_MAX_COUNT] = { 0 };
static uint8_t s_vfs_count = 0;
static bool vfs_initialised = false;
static mutex_t s_fd_table_mutex;

static inline void pico_vfs_table_lock()
{
    mutex_enter_blocking(&s_fd_table_mutex);
}

static inline void pico_vfs_table_unlock()
{
    mutex_exit(&s_fd_table_mutex);
}

int pico_vfs_concat_path(const char *path1, const char *path2,
                         char *dest, size_t destlen)
{
    if (path1[0]=='/' && path1[1]=='\0') {
        path1++;
    }
    return snprintf(dest, destlen, "%s/%s", path1, path2);

}

static const char* translate_path(const pico_vfs_entry_t* vfs, const char* src_path)
{
    assert(strncmp(src_path, vfs->path_prefix, vfs->path_prefix_len) == 0);
    if (strlen(src_path) == vfs->path_prefix_len) {
        // special case when src_path matches the path prefix exactly
        return "/";
    }
    return src_path + vfs->path_prefix_len;
}

static const pico_vfs_entry_t* pico_vfs_get_vfs_entry_for_path(const char* path)
{
    const pico_vfs_entry_t* best_match = NULL;
    ssize_t best_match_prefix_len = -1;
    size_t len = strlen(path);
    //printf("VFS match cnt=%d\n", s_vfs_count);
    for (size_t i = 0; i < s_vfs_count; ++i) {
        const pico_vfs_entry_t* vfs = s_vfs[i];
      /*  if (vfs)
            printf("VFS match len=%d preflen=%d\r\n", len, vfs->path_prefix_len);
        else
            printf("NULL VFS\r\n");
        */
        if (!vfs || vfs->path_prefix_len == LEN_PATH_PREFIX_IGNORED) {
            continue;
        }
          /*
        printf("VFS compare index %d len=%d prefixlen=%d\r\n", i, len, vfs->path_prefix_len);
        printf("VFS  = '%s' '%s'\r\n", path, vfs->path_prefix);
            */

        // match path prefix
        if (len < vfs->path_prefix_len ||
            memcmp(path, vfs->path_prefix, vfs->path_prefix_len) != 0) {
            continue;
        }
        // this is the default VFS and we don't have a better match yet.
        if (vfs->path_prefix_len == 0 && !best_match) {
            best_match = vfs;
            continue;
        }
        // if path is not equal to the prefix, expect to see a path separator
        // i.e. don't match "/data" prefix for "/data1/foo.txt" path
        if (len > vfs->path_prefix_len &&
                path[vfs->path_prefix_len] != '/') {
            continue;
        }
        // Out of all matching path prefixes, select the longest one;
        // i.e. if "/dev" and "/dev/uart" both match, for "/dev/uart/1" path,
        // choose "/dev/uart",
        // This causes all s_vfs_count VFS entries to be scanned when opening
        // a file by name. This can be optimized by introducing a table for
        // FS search order, sorted so that longer prefixes are checked first.
        if (best_match_prefix_len < (ssize_t) vfs->path_prefix_len) {
            best_match_prefix_len = (ssize_t) vfs->path_prefix_len;
            best_match = vfs;
        }
    }
    //printf("VFS: return %s\r\n", best_match ? best_match->path_prefix : NULL);

    return best_match;
}

static inline const pico_vfs_entry_t *pico_vfs_get_vfs_entry_for_index(int index)
{
    if (index < 0 || index >= s_vfs_count) {
        return NULL;
    } else {
        return s_vfs[index];
    }
}

pico_vfs_ops_t *pico_vfs_get_vfs_ops_for_index(int index)
{
    if (index < 0 || index >= s_vfs_count) {
        return NULL;
    } else {
        return &s_vfs[index]->ops;
    }
}


static inline bool pico_vfs_valid_fd(int fd)
{
    return (fd < MAX_FDS) && (fd >= 0);
}

static const pico_vfs_entry_t *pico_vfs_get_vfs_for_fd(int fd)
{
    const pico_vfs_entry_t *vfs = NULL;

    if (pico_vfs_valid_fd(fd)) {
        const int index = s_fd_table[fd].vfs_index;
        vfs = pico_vfs_get_vfs_entry_for_index(index);
    }
    return vfs;
}


static vfs_fd_t pico_vfs_local_fd(const pico_vfs_entry_t *vfs, int global_fd)
{
    vfs_fd_t local_fd = { .fd=-1 };

    if (vfs && pico_vfs_valid_fd(global_fd)) {
        local_fd = s_fd_table[global_fd].local_fd; // single read -> no locking is required
    }

    return local_fd;
}

static int pico_vfs_register_common(const char *base_path, size_t len, const pico_vfs_ops_t *ops, void *drvctx, int *vfs_index)
{
    if (len != LEN_PATH_PREFIX_IGNORED) {
        if ((len != 0 && len < 2) || (len > PICO_VFS_BASE_PATH_MAX)) {
            return EINVAL;
        }
        if ((len > 0 && base_path[0] != '/') || (len>0 && base_path[len - 1] == '/')) {
            return EINVAL;
        }
    }
    pico_vfs_entry_t *entry = (pico_vfs_entry_t*) malloc(sizeof(pico_vfs_entry_t));
    if (entry == NULL) {
        return ENOMEM;
    }
    size_t index;
    for (index = 0; index < s_vfs_count; ++index) {
        if (s_vfs[index] == NULL) {
            break;
        }
    }
    if (index == s_vfs_count) {
        if (s_vfs_count >= VFS_MAX_COUNT) {
            free(entry);
            return ENOMEM;
        }
        ++s_vfs_count;
    }
    s_vfs[index] = entry;

    if (len != LEN_PATH_PREFIX_IGNORED) {
        strcpy(entry->path_prefix, base_path); // we have already verified argument length
    } else {
        bzero(entry->path_prefix, sizeof(entry->path_prefix));
    }
    memcpy(&entry->ops, ops, sizeof(pico_vfs_ops_t));

    entry->path_prefix_len = len;
    entry->index = index;
    entry->drvctx = drvctx;

    if (vfs_index) {
        *vfs_index = index;
    }

    return 0;
}

void __attribute__((weak)) pico_vfs_register_event(const char *base_path)
{
}

void __attribute__((weak)) pico_vfs_deregister_event(const char *base_path)
{
}

vfs_index_t pico_vfs_register(const char* base_path, const pico_vfs_ops_t* vfs, void *drvctx)
{
    int index = -1;
    int r = pico_vfs_register_common(base_path, strlen(base_path), vfs, drvctx, &index);
    if (r<0)
        return r;
    pico_vfs_register_event(base_path);
    return index;
}

int pico_vfs_unregister(vfs_index_t index)
{
    int r = -1;
    char path[PICO_VFS_BASE_PATH_MAX];
    pico_vfs_table_lock();
    pico_vfs_entry_t* vfs = s_vfs[index];
    if (NULL!=vfs) {
        printf("Unregistering %s\n", vfs->path_prefix);
        strcpy( path, vfs->path_prefix );
        free(vfs);
        s_vfs[index] = NULL;
        r = 0;
    }
    pico_vfs_table_unlock();
    if (NULL!=vfs)
    {
        pico_vfs_deregister_event(path);
    }
    return r;
}


int pico_vfs_register_fd_range_for_vfs_index(vfs_index_t index,
                                             int min_fd, int max_fd)
{
    pico_vfs_table_lock();
    for (int i = min_fd; i <= max_fd; ++i) {
        if (s_fd_table[i].vfs_index != -1) {

            for (int j = min_fd; j < i; ++j) {
                if (s_fd_table[j].vfs_index == index) {
                    s_fd_table[j] = FD_TABLE_ENTRY_UNUSED;
                }
            }
            pico_vfs_table_unlock();
            return EINVAL;
        }
        s_fd_table[i].permanent = true;
        s_fd_table[i].vfs_index = index;
        s_fd_table[i].local_fd.fd = i;
    }
    pico_vfs_table_unlock();
    return 0;
}

vfs_index_t pico_vfs_register_fd_range(const pico_vfs_ops_t *vfs, void *drvctx, int min_fd, int max_fd)
{
    if (min_fd < 0 || max_fd < 0 || min_fd > MAX_FDS || max_fd > MAX_FDS || min_fd > max_fd) {
        return EINVAL;
    }

    int index = -1;

    int ret = pico_vfs_register_common("", LEN_PATH_PREFIX_IGNORED, vfs, drvctx, &index);

    if (ret == 0) {
        ret = pico_vfs_register_fd_range_for_vfs_index(index, min_fd, max_fd);
        if (ret !=0) {
#if 0
            pico_vfs_table_lock();
            free(s_vfs[index]);
            s_vfs[index] = NULL;
            pico_vfs_table_unlock();
#endif
            index = -1;
        }
    } else {
        index = -1;
    }

    return index;
}


#define VFSDECL_R(fd, reent) \
    const pico_vfs_entry_t* vfs = pico_vfs_get_vfs_for_fd(fd);   \
    const vfs_fd_t local_fd = pico_vfs_local_fd(vfs, fd);         \
    if (vfs == NULL || local_fd.fd==-1) {                  \
        __errno_r(reent) = EBADF;                       \
        return -1;                                      \
    }

#define VFSCALL_R(rettype, reent, name, ...)  \
    rettype ret; \
    if (vfs->ops.name == NULL) { \
       __errno_r(reent) = ENOSYS; \
       ret = -1; \
    } else { \
      ret = (*vfs->ops.name)( vfs->drvctx, __VA_ARGS__ );  \
      if (ret<0) __errno_r(reent) = -ret; \
    }

#define VFSCALL_R_N(rettype, reent, name, ...)  \
    rettype ret; \
    if (vfs->ops.name == NULL) { \
       __errno_r(reent) = ENOSYS; \
       ret = NULL; \
    } else { \
       ret = (*vfs->ops.name)( vfs->drvctx,  __VA_ARGS__ );  \
    }

#define VFSCALL_V(name, ...)  \
    if (vfs->ops.name != NULL) { \
       (*vfs->ops.name)( vfs->drvctx, __VA_ARGS__ );  \
    }

#define VFS_GENERIC_REENT_CALL(rettype, name, reent, fd, ...) \
    VFSDECL_R(fd, reent); \
    VFSCALL_R(rettype, reent, name, local_fd, __VA_ARGS__); \
    return ret;

#define VFS_GENERIC_REENT_CALL0(rettype, name, reent, fd) \
    VFSDECL_R(fd, reent); \
    VFSCALL_R(rettype, reent, name, local_fd); \
    return ret;

ssize_t pico_vfs_write(struct _reent *r, int fd, const void * data, size_t size)
{

    VFS_GENERIC_REENT_CALL(ssize_t, write, r, fd, data, size);
}

int pico_vfs_close(struct _reent *r, int fd)
{
    VFSDECL_R(fd, r);
    VFSCALL_R(int, r, close, local_fd);

    if (ret == 0) {
        pico_vfs_table_lock();
        if (!s_fd_table[fd].permanent) {
            s_fd_table[fd] = FD_TABLE_ENTRY_UNUSED;
        }
        pico_vfs_table_unlock();
    }

    return ret;
}

ssize_t pico_vfs_read(struct _reent *r, int fd, void * dst, size_t size)
{
    VFS_GENERIC_REENT_CALL(ssize_t, read, r, fd, dst, size);
}

ssize_t pico_vfs_pread(int fd, void *dst, size_t size, off_t offset)
{
    struct _reent* r = __getreent();
    VFS_GENERIC_REENT_CALL(ssize_t, pread, r, fd, dst, size, offset);
}

ssize_t pico_vfs_pwrite(int fd, const void *src, size_t size, off_t offset)
{
    struct _reent* r = __getreent();
    VFS_GENERIC_REENT_CALL(ssize_t, pwrite, r, fd, src, size, offset);
}

off_t pico_vfs_lseek(struct _reent *r, int fd, off_t size, int mode)
{
    VFS_GENERIC_REENT_CALL(off_t, lseek, r, fd, size, mode);
}

int pico_vfs_fcntl(struct _reent *r, int fd, int cmd, int arg)
{
    VFS_GENERIC_REENT_CALL(int, fcntl, r, fd, cmd, arg);
}

int pico_vfs_fstat(struct _reent *r, int fd, struct stat * st)
{
    VFS_GENERIC_REENT_CALL(int, fstat, r, fd, st);
}

int pico_vfs_stat(struct _reent *r, const char *path, struct stat * st)
{
    int ret = -1;
    const pico_vfs_entry_t* vfs = pico_vfs_get_vfs_entry_for_path(path);

    if (vfs == NULL) {
        printf("Cannot open file %s (invalid vfs)\n", path);
        __errno_r(r) = ENOENT;
        return -1;
    }

    const char *path_within_vfs = translate_path(vfs, path);

    if (vfs->ops.stat == NULL) {
        __errno_r(r) = ENOSYS;
        ret = -1;
    } else {
        ret = (*vfs->ops.stat)( vfs->drvctx, path_within_vfs, st );
        if (ret<0) {
            __errno_r(r) = -ret;
        }
    }

    return ret;
}

int pico_vfs_fsync(int fd)
{
    struct _reent* r = __getreent();
    VFS_GENERIC_REENT_CALL0(int, fsync, r, fd);
}

int pico_vfs_ioctl(int fd, int cmd, ...)
{
    struct _reent* r = __getreent();
    va_list ap;
    va_start(ap, cmd);
    VFS_GENERIC_REENT_CALL(int, ioctl, r, fd, cmd, ap);
    va_end(ap);
}

int pico_vfs_open(struct _reent *r, const char * path, int flags, int mode)
{
    printf("Opening %s\n", path);

    const pico_vfs_entry_t* vfs = pico_vfs_get_vfs_entry_for_path(path);

    if (vfs == NULL) {
        printf("Cannot open file %s (invalid vfs)\n", path);
        __errno_r(r) = ENOENT;
        return -1;
    }

    const char *path_within_vfs = translate_path(vfs, path);
    vfs_fd_t fd_within_vfs;
    int ret;

    if (vfs->ops.open == NULL) {
        __errno_r(r) = ENOSYS;
        ret = -1;
    } else {
        ret = (*vfs->ops.open)( vfs->drvctx, &fd_within_vfs, path_within_vfs, flags, mode );
        if (ret<0) {
            __errno_r(r) = -ret;
        }
    }
    /* ret now holds error or the vfs-local fd */

    if (ret == 0)
    {
        pico_vfs_table_lock();

        for (int i = 0; i < MAX_FDS; ++i) {
            if (s_fd_table[i].vfs_index == -1) {
                s_fd_table[i].permanent = false;
                s_fd_table[i].vfs_index = vfs->index;
                s_fd_table[i].local_fd = fd_within_vfs;
                pico_vfs_table_unlock();
                /* Success. Return global fd */
                return i;
            }
        }

        pico_vfs_table_unlock();

        do {
            VFSCALL_R(int, r, close, fd_within_vfs);
            (void)ret;
        } while (0);

        __errno_r(r) = ENFILE;
        return -1;

    } else {
        printf("Cannot open file %s\n", path);
    }
    return ret;
}

DIR* pico_vfs_opendir(const char* name)
{
    const pico_vfs_entry_t * vfs = pico_vfs_get_vfs_entry_for_path(name);

    struct _reent* r = __getreent();
    if (vfs == NULL) {
        __errno_r(r) = ENOENT;
        return NULL;
    }

    const char* path_within_vfs = translate_path(vfs, name);

    VFSCALL_R_N(DIR*, r, opendir,  path_within_vfs);

    if (ret != NULL) {
        ret->vfs_index = vfs->index;
    } else {
        __errno_r(r) = EINVAL; // TBD
    }

    return ret;
}

int pico_vfs_closedir(DIR* pdir)
{
    const pico_vfs_entry_t *vfs = pico_vfs_get_vfs_entry_for_index(pdir->vfs_index);

    struct _reent* r = __getreent();
    if (vfs == NULL) {
        __errno_r(r) = EBADF;
        return -1;
    }
    VFSCALL_R(int, r, closedir, pdir);
    return ret;
}

int pico_vfs_readdir_r(DIR* pdir, struct dirent* entry, struct dirent** out_dirent)
{
    const pico_vfs_entry_t *vfs = pico_vfs_get_vfs_entry_for_index(pdir->vfs_index);

    struct _reent* r = __getreent();
    if (vfs == NULL) {
        __errno_r(r) = EBADF;
        return -1;
    }
    VFSCALL_R(int, r, readdir_r, pdir, entry, out_dirent);

    return ret;
}

struct dirent *pico_vfs_readdir(DIR* pdir)
{
    const pico_vfs_entry_t *vfs = pico_vfs_get_vfs_entry_for_index(pdir->vfs_index);

    struct _reent* r = __getreent();
    if (vfs == NULL) {
        __errno_r(r) = EBADF;
        return NULL;
    }

    VFSCALL_R_N(struct dirent*, r, readdir, pdir);
    return ret;
}


long pico_vfs_telldir(DIR* pdir)
{
    const pico_vfs_entry_t *vfs = pico_vfs_get_vfs_entry_for_index(pdir->vfs_index);

    struct _reent* r = __getreent();
    if (vfs == NULL) {
        __errno_r(r) = EBADF;
        return -1;
    }

    VFSCALL_R(long, r, telldir, pdir);

    return ret;
}

void pico_vfs_seekdir(DIR* pdir, long loc)
{
    const pico_vfs_entry_t *vfs = pico_vfs_get_vfs_entry_for_index(pdir->vfs_index);

    if (vfs == NULL) {
        return;
    }

    VFSCALL_V(seekdir, pdir, loc);

}

static DIR* pico_vfs_root_opendir(void *ctx, const char* name)
{
    //printf("ROOT_OPENDIR: attempt open %s\r\n", name);

    struct _reent* r = __getreent();

    if (name==NULL || name[0]=='\0') {
        __errno_r(r) = ENOENT;
        return NULL;
    }
    if ((name[0] =='/' && name[1]=='\0')) {

        pico_vfs_internal_dir_t *handle = malloc(sizeof(pico_vfs_internal_dir_t));
        handle->iter = 0;
        handle->stream.vfs_index = 0;

        return (DIR*)handle;

    } else {
        __errno_r(r) = ENOENT;
        return NULL;
    }
}

static int pico_vfs_root_closedir(void *ctx, DIR *d)
{
    if (d) {
        free(d);
        return 0;
    }
    return -EINVAL;
}

static struct dirent *pico_vfs_root_readdir(void *ctx, DIR *d)
{
    pico_vfs_internal_dir_t *dir = (pico_vfs_internal_dir_t*)d;
    int cindex = dir->iter;
    pico_vfs_entry_t *vfs = NULL;
    const char *path;

    //printf("ROOT_READDIR\r\n");

    if (cindex >= VFS_MAX_COUNT)
        return NULL;

    do {
        vfs = s_vfs[cindex];
        if ((!vfs) || (vfs->path_prefix[0]=='\0')) {
            vfs = NULL;
            cindex++;
        }
    } while (!vfs && (cindex < VFS_MAX_COUNT));

    if (!vfs) {
        return NULL;
    }
    path = vfs->path_prefix;

    if (path[0]=='/')
        path++;

    //printf("VFS: found entry %d %s\r\n", cindex, path);
    cindex++;
    dir->iter = cindex;

    // Fill in information
    dir->dir.d_type = DT_DIR;
    dir->dir.d_reclen = sizeof(struct dirent);
    strcpy(dir->dir.d_name, path);
    return &dir->dir;
}

vfs_index_t pico_vfs_init(void)
{
    if (vfs_initialised) {
        return EBUSY;
    }
    vfs_initialised = 1;

    mutex_init(&s_fd_table_mutex);

    pico_vfs_table_lock();

    for (int i = 0; i < MAX_FDS; ++i) {
        s_fd_table[i].vfs_index = -1;
    }

    pico_vfs_table_unlock();

    // Init root VFS, mainly for root directory iteration.
    const pico_vfs_ops_t rootops =
    {
        .opendir  = &pico_vfs_root_opendir,
        .closedir = &pico_vfs_root_closedir,
        .readdir = &pico_vfs_root_readdir
    };
    int index = -1;
    int ret = pico_vfs_register_common("",
                                       0,
                                       &rootops,
                                       NULL,
                                       &index);

    if (ret<0)
        return ret;

    return index;
}

void pico_vfs_lock_init(vfs_lock_t *lock)
{
    mutex_init(lock);
}

void pico_vfs_lock_acquire(vfs_lock_t *lock)
{
    mutex_enter_blocking(lock);
}

void pico_vfs_lock_release(vfs_lock_t *lock)
{
    mutex_exit(lock);

}

#if LIB_PICO_STDIO
#include <pico/stdio.h>

extern int _read(int handle, char *buffer, int length);
extern int _write(int handle, char *buffer, int length);

ssize_t stdio_vfs_write(void *ctx, vfs_fd_t fd, const void *buffer, size_t length)
{
    return _read( fd.fd, (void*)buffer, length );
}

ssize_t stdio_vfs_read(void *ctx, vfs_fd_t fd, void *buffer, size_t length)
{
    return _write( fd.fd, buffer, length );
}

void stdio_vfs_init()
{
    vfs_index_t rootindex = pico_vfs_init();

    if (rootindex == 0)
    {
        if (pico_vfs_register_fd_range_for_vfs_index(rootindex, 0, 1) ==0)
        {
            pico_vfs_ops_t *ops = pico_vfs_get_vfs_ops_for_index(rootindex);

            if (ops) {
                ops->read = &stdio_vfs_read;
                ops->write = &stdio_vfs_write;
            }
        }
    }
}
#endif

/*
 Aliases for our VFS functions. These implement the newlib syscalls
 */

int _open_r(struct _reent *r, const char * path, int flags, int mode) __attribute__((alias("pico_vfs_open")));
int _close_r(struct _reent *r, int fd) __attribute__((alias("pico_vfs_close")));
ssize_t _read_r(struct _reent *r, int fd, void * dst, size_t size) __attribute__((alias("pico_vfs_read")));
ssize_t _write_r(struct _reent *r, int fd, const void * data, size_t size) __attribute__((alias("pico_vfs_write")));
ssize_t pread(int fd, void *dst, size_t size, off_t offset) __attribute__((alias("pico_vfs_pread")));
ssize_t pwrite(int fd, const void *src, size_t size, off_t offset) __attribute__((alias("pico_vfs_pwrite")));
off_t _lseek_r(struct _reent *r, int fd, off_t size, int mode) __attribute__((alias("pico_vfs_lseek")));
int _fcntl_r(struct _reent *r, int fd, int cmd, int arg) __attribute__((alias("pico_vfs_fcntl")));
int _fstat_r(struct _reent *r, int fd, struct stat * st) __attribute__((alias("pico_vfs_fstat")));
int _stat_r(struct _reent *r, const char *, struct stat * st) __attribute__((alias("pico_vfs_stat")));
int fsync(int fd) __attribute__((alias("pico_vfs_fsync")));
int ioctl(int fd, int cmd, ...) __attribute__((alias("pico_vfs_ioctl")));


DIR* opendir(const char* name) __attribute__((alias("pico_vfs_opendir")));
int closedir(DIR* pdir) __attribute__((alias("pico_vfs_closedir")));
int readdir_r(DIR* pdir, struct dirent* entry, struct dirent** out_dirent) __attribute__((alias("pico_vfs_readdir_r")));
struct dirent* readdir(DIR* pdir) __attribute__((alias("pico_vfs_readdir")));
long telldir(DIR* pdir) __attribute__((alias("pico_vfs_telldir")));
void seekdir(DIR* pdir, long loc) __attribute__((alias("pico_vfs_seekdir")));


