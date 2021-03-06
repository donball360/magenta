// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <utime.h>
#include <threads.h>
#include <unistd.h>

#include <magenta/processargs.h>
#include <magenta/syscalls.h>

#include <mxio/debug.h>
#include <mxio/io.h>
#include <mxio/private.h>
#include <mxio/remoteio.h>
#include <mxio/util.h>
#include <mxio/vfs.h>
#include <mxio/socket.h>

#include "private.h"
#include "unistd.h"

static_assert(MXIO_FLAG_CLOEXEC == FD_CLOEXEC, "Unexpected mxio flags value");

// non-thread-safe emulation of unistd io functions
// using the mxio transports

mxio_state_t __mxio_global_state = {
    .lock = MTX_INIT,
    .cwd_lock = MTX_INIT,
    .init = true,
    .cwd_path = "/",
};

void mxio_install_root(mxio_t* root) {
    mtx_lock(&mxio_lock);
    if (mxio_root_init) {
        mxio_root_handle = root;
        mxio_root_init = false;
    }
    mtx_unlock(&mxio_lock);
}

// Attaches an mxio to an fdtab slot.
// The mxio must have been upref'd on behalf of the
// fdtab prior to binding.
int mxio_bind_to_fd(mxio_t* io, int fd, int starting_fd) {
    mxio_t* io_to_close = NULL;

    mtx_lock(&mxio_lock);
    if (fd < 0) {
        // A negative fd implies that any free fd value can be used
        //TODO: bitmap, ffs, etc
        for (fd = starting_fd; fd < MAX_MXIO_FD; fd++) {
            if (mxio_fdtab[fd] == NULL) {
                goto free_fd_found;
            }
        }
        errno = EMFILE;
        mtx_unlock(&mxio_lock);
        return -1;
    } else if (fd >= MAX_MXIO_FD) {
        errno = EINVAL;
        mtx_unlock(&mxio_lock);
        return -1;
    } else {
        io_to_close = mxio_fdtab[fd];
        if (io_to_close) {
            io_to_close->dupcount--;
            if (io_to_close->dupcount > 0) {
                // still alive in another fdtab slot
                mxio_release(io_to_close);
                io_to_close = NULL;
            }
        }
    }

free_fd_found:
    io->dupcount++;
    mxio_fdtab[fd] = io;
    mtx_unlock(&mxio_lock);

    if (io_to_close) {
        io_to_close->ops->close(io_to_close);
        mxio_release(io_to_close);
    }
    return fd;
}

// If a mxio_t exists for this fd and it has not been dup'd
// and is not in active use (an io operation underway, etc),
// detach it from the fdtab and return it with a single
// refcount.
mx_status_t mxio_unbind_from_fd(int fd, mxio_t** out) {
    mx_status_t status;
    mtx_lock(&mxio_lock);
    if (fd >= MAX_MXIO_FD) {
        status = ERR_INVALID_ARGS;
        goto done;
    }
    mxio_t* io = mxio_fdtab[fd];
    if (io == NULL) {
        status = ERR_INVALID_ARGS;
        goto done;
    }
    if (io->dupcount > 1) {
        status = ERR_UNAVAILABLE;
        goto done;
    }
    if (atomic_load(&io->refcount) > 1) {
        status = ERR_UNAVAILABLE;
        goto done;
    }
    io->dupcount = 0;
    mxio_fdtab[fd] = NULL;
    *out = io;
    status = NO_ERROR;
done:
    mtx_unlock(&mxio_lock);
    return status;
}

mxio_t* __mxio_fd_to_io(int fd) {
    if ((fd < 0) || (fd >= MAX_MXIO_FD)) {
        return NULL;
    }
    mxio_t* io = NULL;
    mtx_lock(&mxio_lock);
    if ((io = mxio_fdtab[fd]) != NULL) {
        mxio_acquire(io);
    }
    mtx_unlock(&mxio_lock);
    return io;
}

static void mxio_exit(void) {
    mtx_lock(&mxio_lock);
    for (int fd = 0; fd < MAX_MXIO_FD; fd++) {
        mxio_t* io = mxio_fdtab[fd];
        if (io) {
            mxio_fdtab[fd] = NULL;
            io->dupcount--;
            if (io->dupcount == 0) {
                io->ops->close(io);
                mxio_release(io);
            }
        }
    }
    mtx_unlock(&mxio_lock);
}

mx_status_t mxio_close(mxio_t* io) {
    if (io->dupcount > 0) {
        printf("mxio_close(%p): dupcount nonzero!\n", io);
    }
    return io->ops->close(io);
}

// Possibly return an owned mxio_t corresponding to either the root,
// the cwd, or, for the ...at variants, dirfd. In the absolute path
// case, *path is also adjusted.
static mxio_t* mxio_iodir(const char** path, int dirfd) {
    mxio_t* iodir = NULL;
    mtx_lock(&mxio_lock);
    if (*path[0] == '/') {
        iodir = mxio_root_handle;
        (*path)++;
        if (*path[0] == 0) {
            *path = ".";
        }
    } else if (dirfd == AT_FDCWD) {
        iodir = mxio_cwd_handle;
    } else if ((dirfd >= 0) && (dirfd < MAX_MXIO_FD)) {
        iodir = mxio_fdtab[dirfd];
    }
    if (iodir != NULL) {
        mxio_acquire(iodir);
    }
    mtx_unlock(&mxio_lock);
    return iodir;
}

static mx_status_t __mxio_open_at(mxio_t** io, int dirfd, const char* path, int flags, uint32_t mode) {
    if (path == NULL) {
        return ERR_INVALID_ARGS;
    }
    if (path[0] == 0) {
        return ERR_INVALID_ARGS;
    }

    mxio_t* iodir = mxio_iodir(&path, dirfd);
    if (iodir == NULL) {
        return ERR_BAD_HANDLE;
    }

    mx_status_t r = iodir->ops->open(iodir, path, flags, mode, io);
    mxio_release(iodir);
    return r;
}

mx_status_t __mxio_open(mxio_t** io, const char* path, int flags, uint32_t mode) {
    return __mxio_open_at(io, AT_FDCWD, path, flags, mode);
}

static void update_cwd_path(const char* path) {
    if (path[0] == '/') {
        // it's "absolute", but we'll still parse it as relative (from /)
        // so that we normalize the path (resolving, ., .., //, etc)
        mxio_cwd_path[0] = '/';
        mxio_cwd_path[1] = 0;
        path++;
    }

    size_t seglen;
    const char* next;
    for (; path[0]; path = next) {
        next = strchr(path, '/');
        if (next == NULL) {
            seglen = strlen(path);
            next = path + seglen;
        } else {
            seglen = next - path;
            next++;
        }
        if (seglen == 0) {
            // empty segment, skip
            continue;
        }
        if ((seglen == 1) && (path[0] == '.')) {
            // no-change segment, skip
            continue;
        }
        if ((seglen == 2) && (path[0] == '.') && (path[1] == '.')) {
            // parent directory, remove the trailing path segment from cwd_path
            char* x = strrchr(mxio_cwd_path, '/');
            if (x == NULL) {
                // shouldn't ever happen
                goto wat;
            }
            // remove the current trailing path segment from cwd
            if (x == mxio_cwd_path) {
                // but never remove the first /
                mxio_cwd_path[1] = 0;
            } else {
                x[0] = 0;
            }
            continue;
        }
        // regular path segment, append to cwd_path
        size_t len = strlen(mxio_cwd_path);
        if ((len + seglen + 2) >= PATH_MAX) {
            // doesn't fit, shouldn't happen, but...
            goto wat;
        }
        if (len != 1) {
            // if len is 1, path is "/", so don't append a '/'
            mxio_cwd_path[len++] = '/';
        }
        memcpy(mxio_cwd_path + len, path, seglen);
        mxio_cwd_path[len + seglen] = 0;
    }
    return;

wat:
    strcpy(mxio_cwd_path, "(unknown)");
    return;
}

// opens the directory containing path
// returns the non-directory portion of the path as name on success
static mx_status_t __mxio_opendir_containing_at(mxio_t** io, int dirfd, const char* path, const char** _name) {
    if (path == NULL) {
        return ERR_INVALID_ARGS;
    }

    mxio_t* iodir = mxio_iodir(&path, dirfd);
    if (iodir == NULL) {
        return ERR_BAD_HANDLE;
    }

    char dirpath[PATH_MAX];

    // Make 'path_end' the final index of the string without trailing '/' characters.
    size_t path_end = strnlen(path, PATH_MAX - 1) - 1;
    while ((path_end > 0) && (path[path_end] == '/')) {
        path_end--;
    }

    // Find the last non-trailing '/'
    const char* name = path + path_end;
    while ((name > path) && (*name != '/')) {
        name--;
    }

    if ((name == path) && (*name != '/')) {
        // No '/' characters found
        name = path;
        dirpath[0] = '.';
        dirpath[1] = 0;
    } else {
        // At least one '/' found
        if ((name - path) > (ptrdiff_t)(sizeof(dirpath) - 1)) {
            mxio_release(iodir);
            return ERR_INVALID_ARGS;
        }
        memcpy(dirpath, path, name - path);
        dirpath[name - path] = 0;
        name++;
    }
    if (name[0] == 0) {
        mxio_release(iodir);
        return ERR_INVALID_ARGS;
    }

    *_name = name;
    mx_status_t r = iodir->ops->open(iodir, dirpath, O_DIRECTORY, 0, io);

    mxio_release(iodir);
    return r;
}

static mx_status_t __mxio_opendir_containing(mxio_t** io, const char* path, const char** _name) {
    return __mxio_opendir_containing_at(io, AT_FDCWD, path, _name);
}

// hook into libc process startup
// this is called prior to main to set up the mxio world
// and thus does not use the mxio_lock
void __libc_extensions_init(uint32_t handle_count,
                            mx_handle_t handle[],
                            uint32_t handle_info[]) {
    int stdio_fd = -1;

    // extract handles we care about
    for (uint32_t n = 0; n < handle_count; n++) {
        unsigned arg = MX_HND_INFO_ARG(handle_info[n]);
        mx_handle_t h = handle[n];

        // MXIO uses this bit as a flag to say
        // that an fd should be duped into 0/1/2
        // and become all of stdin/out/err
        if (arg & MXIO_FLAG_USE_FOR_STDIO) {
            arg &= (~MXIO_FLAG_USE_FOR_STDIO);
            if (arg < MAX_MXIO_FD) {
                stdio_fd = arg;
            }
        }

        switch (MX_HND_INFO_TYPE(handle_info[n])) {
        case MX_HND_TYPE_MXIO_ROOT:
            mxio_root_handle = mxio_remote_create(h, 0);
            break;
        case MX_HND_TYPE_MXIO_CWD:
            mxio_cwd_handle = mxio_remote_create(h, 0);
            break;
        case MX_HND_TYPE_MXIO_REMOTE:
            // remote objects may have a second handle
            // which is for signaling events
            if (((n + 1) < handle_count) &&
                (handle_info[n] == handle_info[n + 1])) {
                mxio_fdtab[arg] = mxio_remote_create(h, handle[n + 1]);
                handle_info[n + 1] = 0;
            } else {
                mxio_fdtab[arg] = mxio_remote_create(h, 0);
            }
            mxio_fdtab[arg]->dupcount++;
            break;
        case MX_HND_TYPE_MXIO_PIPE:
            mxio_fdtab[arg] = mxio_pipe_create(h);
            mxio_fdtab[arg]->dupcount++;
            break;
        case MX_HND_TYPE_MXIO_LOGGER:
            mxio_fdtab[arg] = mxio_logger_create(h);
            mxio_fdtab[arg]->dupcount++;
            break;
        default:
            // unknown handle, leave it alone
            continue;
        }
        handle[n] = 0;
        handle_info[n] = 0;
    }

    // Set up thread local storage for rchannels.
    __mxio_rchannel_init();

    // TODO(abarth): The cwd path string should be more tightly coupled with
    // the cwd handle.
    const char* cwd = getenv("PWD");
    if (cwd != NULL) {
        update_cwd_path(cwd);
    }

    mxio_t* use_for_stdio = (stdio_fd >= 0) ? mxio_fdtab[stdio_fd] : NULL;

    // configure stdin/out/err if not init'd
    for (uint32_t n = 0; n < 3; n++) {
        if (mxio_fdtab[n] == NULL) {
            if (use_for_stdio) {
                mxio_fdtab[n] = use_for_stdio;
            } else {
                mxio_fdtab[n] = mxio_null_create();
            }
            mxio_fdtab[n]->dupcount++;
        }
    }

    if (mxio_root_handle) {
        mxio_root_init = true;
        if(!mxio_cwd_handle) {
            __mxio_open(&mxio_cwd_handle, mxio_cwd_path, O_DIRECTORY, 0);
        }
    } else {
        // placeholder null handle
        mxio_root_handle = mxio_null_create();
    }
    if (mxio_cwd_handle == NULL) {
        mxio_cwd_handle = mxio_null_create();
    }

    atexit(mxio_exit);
}

mx_status_t mxio_clone_root(mx_handle_t* handles, uint32_t* types) {
    // The root handle is established in the init hook called from
    // libc startup (or, in the special case of devmgr, installed
    // slightly later), and is never NULL and does not change
    // in normal operation
    mx_status_t r = mxio_root_handle->ops->clone(mxio_root_handle, handles, types);
    if (r > 0) {
        *types = MX_HND_TYPE_MXIO_ROOT;
    }
    return r;
}

mx_status_t mxio_clone_cwd(mx_handle_t* handles, uint32_t* types) {
    mx_status_t r = mxio_cwd_handle->ops->clone(mxio_cwd_handle, handles, types);
    if (r > 0) {
        *types = MX_HND_TYPE_MXIO_CWD;
    }
    return r;
}

mx_status_t mxio_clone_fd(int fd, int newfd, mx_handle_t* handles, uint32_t* types) {
    mx_status_t r;
    mxio_t* io;
    if ((io = fd_to_io(fd)) == NULL) {
        return ERR_BAD_HANDLE;
    }
    //TODO: implement/honor close-on-exec flag
    if ((r = io->ops->clone(io, handles, types)) > 0) {
        for (int i = 0; i < r; i++) {
            types[i] |= (newfd << 16);
        }
    }
    mxio_release(io);
    return r;
}

mx_status_t mxio_transfer_fd(int fd, int newfd, mx_handle_t* handles, uint32_t* types) {
    mxio_t* io;
    mx_status_t status;
    if ((status = mxio_unbind_from_fd(fd, &io)) < 0) {
        return status;
    }
    if ((status = io->ops->unwrap(io, handles, types)) < 0) {
        return status;
    }
    for (int n = 0; n < status; n++) {
        types[n] |= (newfd << 16);
    }
    return status;
}

ssize_t mxio_ioctl(int fd, int op, const void* in_buf, size_t in_len, void* out_buf, size_t out_len) {
    mxio_t* io;
    if ((io = fd_to_io(fd)) == NULL) {
        return ERR_BAD_HANDLE;
    }
    ssize_t r = io->ops->ioctl(io, op, in_buf, in_len, out_buf, out_len);
    mxio_release(io);
    return r;
}

mx_status_t mxio_get_vmo(int fd, mx_handle_t* vmo, size_t* off, size_t* len) {
    mxio_t* io;
    if ((io = fd_to_io(fd)) == NULL) {
        return ERR_BAD_HANDLE;
    }
    mx_status_t r = io->ops->get_vmo(io, vmo, off, len);
    mxio_release(io);
    return r;
}

mx_status_t mxio_wait_fd(int fd, uint32_t events, uint32_t* _pending, mx_time_t timeout) {
    mx_status_t r = NO_ERROR;
    mxio_t* io;
    if ((io = fd_to_io(fd)) == NULL) {
        return ERR_BAD_HANDLE;
    }

    mx_handle_t h = MX_HANDLE_INVALID;
    mx_signals_t signals = 0;
    io->ops->wait_begin(io, events, &h, &signals);
    if (h == MX_HANDLE_INVALID) {
        // wait operation is not applicable to the handle
        r = ERR_INVALID_ARGS;
        goto end;
    }
    mx_signals_t pending;
    if ((r = mx_object_wait_one(h, signals, timeout, &pending)) < 0) {
        if (r != ERR_TIMED_OUT) {
            goto end;
        }
    }
    io->ops->wait_end(io, pending, &events);

    if (_pending) {
        *_pending = events;
    }
 end:
    mxio_release(io);
    return r;
}

int mxio_stat(mxio_t* io, struct stat* s) {
    vnattr_t attr;
    int r = io->ops->misc(io, MXRIO_STAT, 0, sizeof(attr), &attr, 0);
    if (r < 0) {
        return ERR_BAD_HANDLE;
    }
    if (r < (int)sizeof(attr)) {
        return ERR_IO;
    }
    memset(s, 0, sizeof(struct stat));
    s->st_mode = attr.mode;
    s->st_ino = attr.inode;
    s->st_size = attr.size;
    s->st_nlink = attr.nlink;
    s->st_ctim.tv_sec = attr.create_time / MX_SEC(1);
    s->st_ctim.tv_nsec = attr.create_time % MX_SEC(1);
    s->st_mtim.tv_sec = attr.modify_time / MX_SEC(1);
    s->st_mtim.tv_nsec = attr.modify_time % MX_SEC(1);
    return 0;
}


mx_status_t mxio_setattr(mxio_t* io, vnattr_t* vn){
    mx_status_t r = io->ops->misc(io, MXRIO_SETATTR, 0, 0, vn, sizeof(*vn));
    if (r < 0) {
        return ERR_BAD_HANDLE;
    }

    return  r;
}


// TODO: determine complete correct mapping
int mxio_status_to_errno(mx_status_t status) {
    switch (status) {
    case ERR_NOT_FOUND: return ENOENT;
    case ERR_NO_MEMORY: return ENOMEM;
    case ERR_INVALID_ARGS: return EINVAL;
    case ERR_BUFFER_TOO_SMALL: return EINVAL;
    case ERR_TIMED_OUT: return ETIMEDOUT;
    case ERR_ALREADY_EXISTS: return EEXIST;
    case ERR_REMOTE_CLOSED: return ENOTCONN;
    case ERR_BAD_PATH: return ENAMETOOLONG;
    case ERR_IO: return EIO;
    case ERR_NOT_DIR: return ENOTDIR;
    case ERR_NOT_SUPPORTED: return ENOTSUP;
    case ERR_OUT_OF_RANGE: return EINVAL;
    case ERR_NO_RESOURCES: return ENOMEM;
    case ERR_BAD_HANDLE: return EBADF;
    case ERR_ACCESS_DENIED: return EACCES;
    case ERR_SHOULD_WAIT: return EAGAIN;
    case ERR_FILE_BIG: return EFBIG;
    case ERR_NO_SPACE: return ENOSPC;

    // No specific translation, so return a generic errno value.
    default: return EIO;
    }
}

// The functions from here on provide implementations of fd and path
// centric posix-y io operations.

ssize_t readv(int fd, const struct iovec* iov, int num) {
    ssize_t count = 0;
    ssize_t r;
    while (num > 0) {
        if (iov->iov_len != 0) {
            r = read(fd, iov->iov_base, iov->iov_len);
            if (r < 0) {
                return count ? count : r;
            }
            if ((size_t)r < iov->iov_len) {
                return count + r;
            }
            count += r;
        }
        iov++;
        num--;
    }
    return count;
}

ssize_t writev(int fd, const struct iovec* iov, int num) {
    ssize_t count = 0;
    ssize_t r;
    while (num > 0) {
        if (iov->iov_len != 0) {
            r = write(fd, iov->iov_base, iov->iov_len);
            if (r < 0) {
                return count ? count : r;
            }
            if ((size_t)r < iov->iov_len) {
                return count + r;
            }
            count += r;
        }
        iov++;
        num--;
    }
    return count;
}

int unlinkat(int dirfd, const char* path, int flags) {
    const char* name;
    mxio_t* io;
    mx_status_t r;
    if ((r = __mxio_opendir_containing_at(&io, dirfd, path, &name)) < 0) {
        return ERROR(r);
    }
    r = io->ops->misc(io, MXRIO_UNLINK, 0, 0, (void*)name, strlen(name));
    io->ops->close(io);
    mxio_release(io);
    return STATUS(r);
}

ssize_t read(int fd, void* buf, size_t count) {
    if (buf == NULL) {
        return ERRNO(EINVAL);
    }

    mxio_t* io = fd_to_io(fd);
    if (io == NULL) {
        return ERRNO(EBADF);
    }
    mx_status_t status;
    for (;;) {
        status = io->ops->read(io, buf, count);
        if (status != ERR_SHOULD_WAIT || io->flags & MXIO_FLAG_NONBLOCK) {
            break;
        }
        mxio_wait_fd(fd, MXIO_EVT_READABLE, NULL, MX_TIME_INFINITE);
    }
    mxio_release(io);
    return STATUS(status);
}

ssize_t write(int fd, const void* buf, size_t count) {
    if (buf == NULL) {
        return ERRNO(EINVAL);
    }

    mxio_t* io = fd_to_io(fd);
    if (io == NULL) {
        return ERRNO(EBADF);
    }
    mx_status_t status;
    for (;;) {
        status = io->ops->write(io, buf, count);
        if (status != ERR_SHOULD_WAIT || io->flags & MXIO_FLAG_NONBLOCK) {
            break;
        }
        mxio_wait_fd(fd, MXIO_EVT_WRITABLE, NULL, MX_TIME_INFINITE);
    }
    mxio_release(io);
    return STATUS(status);
}

ssize_t preadv(int fd, const struct iovec* iov, int count, off_t ofs) {
    ssize_t iov_count = 0;
    ssize_t r;
    while (count > 0) {
        if (iov->iov_len != 0) {
            r = pread(fd, iov->iov_base, iov->iov_len, ofs);
            if (r < 0) {
                return iov_count ? iov_count : r;
            }
            if ((size_t)r < iov->iov_len) {
                return iov_count + r;
            }
            iov_count += r;
            ofs += r;
        }
        iov++;
        count--;
    }
    return iov_count;
}

ssize_t pread(int fd, void* buf, size_t size, off_t ofs) {
    if (buf == NULL) {
        return ERRNO(EINVAL);
    }

    mxio_t* io = fd_to_io(fd);
    if (io == NULL) {
        return ERRNO(EBADF);
    }
    mx_status_t status;
    for (;;) {
        status = io->ops->read_at(io, buf, size, ofs);
        if (status != ERR_SHOULD_WAIT || io->flags & MXIO_FLAG_NONBLOCK) {
            break;
        }
        mxio_wait_fd(fd, MXIO_EVT_READABLE, NULL, MX_TIME_INFINITE);
    }
    mxio_release(io);
    return STATUS(status);
}

ssize_t pwritev(int fd, const struct iovec* iov, int count, off_t ofs) {
    ssize_t iov_count = 0;
    ssize_t r;
    while (count > 0) {
        if (iov->iov_len != 0) {
            r = pwrite(fd, iov->iov_base, iov->iov_len, ofs);
            if (r < 0) {
                return iov_count ? iov_count : r;
            }
            if ((size_t)r < iov->iov_len) {
                return iov_count + r;
            }
            iov_count += r;
            ofs += r;
        }
        iov++;
        count--;
    }
    return iov_count;
}

ssize_t pwrite(int fd, const void* buf, size_t size, off_t ofs) {
    if (buf == NULL) {
        return ERRNO(EINVAL);
    }

    mxio_t* io = fd_to_io(fd);
    if (io == NULL) {
        return ERRNO(EBADF);
    }
    mx_status_t status;
    for (;;) {
        status = io->ops->write_at(io, buf, size, ofs);
        if (status != ERR_SHOULD_WAIT || io->flags & MXIO_FLAG_NONBLOCK) {
            break;
        }
        mxio_wait_fd(fd, MXIO_EVT_WRITABLE, NULL, MX_TIME_INFINITE);
    }
    mxio_release(io);
    return STATUS(status);
}

int close(int fd) {
    mtx_lock(&mxio_lock);
    if ((fd < 0) || (fd >= MAX_MXIO_FD) || (mxio_fdtab[fd] == NULL)) {
        mtx_unlock(&mxio_lock);
        return ERRNO(EBADF);
    }
    mxio_t* io = mxio_fdtab[fd];
    io->dupcount--;
    mxio_fdtab[fd] = NULL;
    if (io->dupcount > 0) {
        // still alive in other fdtab slots
        mtx_unlock(&mxio_lock);
        mxio_release(io);
        return NO_ERROR;
    } else {
        mtx_unlock(&mxio_lock);
        int r = io->ops->close(io);
        mxio_release(io);
        return STATUS(r);
    }
}

static int mxio_dup(int oldfd, int newfd, int starting_fd) {
    mxio_t* io = fd_to_io(oldfd);
    if (io == NULL) {
        return ERRNO(EBADF);
    }
    int fd = mxio_bind_to_fd(io, newfd, starting_fd);
    if (fd < 0) {
        mxio_release(io);
    }
    return fd;
}

int dup2(int oldfd, int newfd) {
    return mxio_dup(oldfd, newfd, 0);
}

int dup(int oldfd) {
    return mxio_dup(oldfd, -1, 0);
}

int dup3(int oldfd, int newfd, int flags) {
    // dup3 differs from dup2 in that it fails with EINVAL, rather
    // than being a no op, on being given the same fd for both old and
    // new.
    if (oldfd == newfd) {
        return ERRNO(EINVAL);
    }

    if (flags != 0 && flags != O_CLOEXEC) {
        return ERRNO(EINVAL);
    }

    // TODO(kulakowski) Implement O_CLOEXEC.
    return mxio_dup(oldfd, newfd, 0);
}

int fcntl(int fd, int cmd, ...) {
// Note that it is not safe to pull out the int out of the
// variadic arguments at the top level, as callers are not
// required to pass anything for many of the commands.
#define GET_INT_ARG(ARG)         \
    va_list args;                \
    va_start(args, cmd);         \
    int ARG = va_arg(args, int); \
    va_end(args)

    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        // TODO(kulakowski) Implement CLOEXEC.
        GET_INT_ARG(starting_fd);
        return mxio_dup(fd, -1, starting_fd);
    }
    case F_GETFD: {
        mxio_t* io = fd_to_io(fd);
        if (io == NULL) {
            return ERRNO(EBADF);
        }
        int flags = (int)(io->flags & MXIO_FD_FLAGS);
        // POSIX mandates that the return value be nonnegative if successful.
        assert(flags >= 0);
        mxio_release(io);
        return flags;
    }
    case F_SETFD: {
        mxio_t* io = fd_to_io(fd);
        if (io == NULL) {
            return ERRNO(EBADF);
        }
        GET_INT_ARG(flags);
        // TODO(kulakowski) Implement CLOEXEC.
        io->flags &= ~MXIO_FD_FLAGS;
        io->flags |= (int32_t)flags & MXIO_FD_FLAGS;
        mxio_release(io);
        return 0;
    }
    case F_GETFL: {
        // TODO(kulakowski) File status flags and access modes.
        mxio_t* io = fd_to_io(fd);
        if (io == NULL) {
            return ERRNO(EBADF);
        }
        int status = 0;
        if (io->flags & MXIO_FLAG_NONBLOCK) {
            status |= O_NONBLOCK;
        }
        mxio_release(io);
        return status;
    }
    case F_SETFL: {
        // TODO(kulakowski) File status flags and access modes.
        mxio_t* io = fd_to_io(fd);
        if (io == NULL) {
            return ERRNO(EBADF);
        }
        GET_INT_ARG(status);
        if (status & O_NONBLOCK) {
            io->flags |= MXIO_FLAG_NONBLOCK;
        } else {
            io->flags &= ~MXIO_FLAG_NONBLOCK;
        }
        mxio_release(io);
        return 0;
    }
    case F_GETOWN:
    case F_SETOWN:
        // TODO(kulakowski) Socket support.
        return ERRNO(ENOSYS);
    case F_GETLK:
    case F_SETLK:
    case F_SETLKW:
        // TODO(kulakowski) Advisory file locking support.
        return ERRNO(ENOSYS);
    default:
        return ERRNO(EINVAL);
    }

#undef GET_INT_ARG
}

off_t lseek(int fd, off_t offset, int whence) {
    mxio_t* io = fd_to_io(fd);
    if (io == NULL) {
        return ERRNO(EBADF);
    }
    off_t r = io->ops->seek(io, offset, whence);
    if (r < 0) {
        r = ERROR(r);
    }
    mxio_release(io);
    return r;
}

static int getdirents(int fd, void* ptr, size_t len, long cmd) {
    mxio_t* io = fd_to_io(fd);
    if (io == NULL) {
        return ERRNO(EBADF);
    }
    int r = STATUS(io->ops->misc(io, MXRIO_READDIR, cmd, len, ptr, 0));
    mxio_release(io);
    return r;
}

static int truncateat(int dirfd, const char* path, off_t len) {
    mxio_t* io;
    mx_status_t r;

    if ((r = __mxio_open_at(&io, dirfd, path, O_WRONLY, 0)) < 0) {
        return ERROR(r);
    }
    r = STATUS(io->ops->misc(io, MXRIO_TRUNCATE, len, 0, NULL, 0));
    mxio_close(io);
    mxio_release(io);
    return STATUS(r);
}

int truncate(const char* path, off_t len) {
    return truncateat(AT_FDCWD, path, len);
}

int ftruncate(int fd, off_t len) {
    mxio_t* io = fd_to_io(fd);
    if (io == NULL) {
        return ERRNO(EBADF);
    }
    int r = STATUS(io->ops->misc(io, MXRIO_TRUNCATE, len, 0, NULL, 0));
     mxio_release(io);
     return r;
}

static int two_path_op(uint32_t op, const char* oldpath, const char* newpath) {
    char name[MXIO_CHUNK_SIZE];
    size_t oldlen = strlen(oldpath);
    size_t newlen = strlen(newpath);
    if (oldlen + newlen + 2 > MXIO_CHUNK_SIZE) {
        return ERRNO(EINVAL);
    }

    mxio_t* io;
    if (oldpath[0] == '/' && newpath[0] == '/') {
        // Both paths are absolute: Op relative to mxio_root
        mtx_lock(&mxio_lock);
        io = mxio_root_handle;
        mxio_acquire(io);
        mtx_unlock(&mxio_lock);
    } else if (oldpath[0] != '/' && newpath[0] != '/') {
        // Both paths are relative: Op relative to mxio_cwd
        mtx_lock(&mxio_lock);
        io = mxio_cwd_handle;
        mxio_acquire(io);
        mtx_unlock(&mxio_lock);
    } else {
        // Mixed absolute & relative paths: Unsupported
        return ERROR(ERR_NOT_SUPPORTED);
    }

    memcpy(name, oldpath, oldlen);
    name[oldlen] = '\0';
    memcpy(name + oldlen + 1, newpath, newlen);
    name[oldlen + newlen + 1] = '\0';
    mx_status_t r = io->ops->misc(io, op, 0, 0, (void*)name, oldlen + newlen + 2);
    mxio_release(io);
    return STATUS(r);
}

int rename(const char* oldpath, const char* newpath) {
    return two_path_op(MXRIO_RENAME, oldpath, newpath);
}

int link(const char* oldpath, const char* newpath) {
    return two_path_op(MXRIO_LINK, oldpath, newpath);
}

int unlink(const char* path) {
    return unlinkat(AT_FDCWD, path, 0);
}

static int vopenat(int dirfd, const char* path, int flags, va_list args) {
    mxio_t* io = NULL;
    mx_status_t r;
    int fd;
    uint32_t mode = 0;

    if (flags & O_CREAT) {
        if (flags & O_DIRECTORY) {
            // The behavior of open with O_CREAT | O_DIRECTORY is underspecified
            // in POSIX. To help avoid programmer error, we explicitly disallow
            // the combination.
            return ERRNO(EINVAL);
        }
        mode = va_arg(args, uint32_t) & 0777;
    }
    if ((r = __mxio_open_at(&io, dirfd, path, flags, mode)) < 0) {
        return ERROR(r);
    }
    if (flags & O_NONBLOCK) {
        io->flags |= MXIO_FLAG_NONBLOCK;
    }
    if ((fd = mxio_bind_to_fd(io, -1, 0)) < 0) {
        io->ops->close(io);
        mxio_release(io);
        return ERRNO(EMFILE);
    }
    return fd;
}

int open(const char* path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    int ret = vopenat(AT_FDCWD, path, flags, ap);
    va_end(ap);
    return ret;
}

int openat(int dirfd, const char* path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    int ret = vopenat(dirfd, path, flags, ap);
    va_end(ap);
    return ret;
}

int mkdir(const char* path, mode_t mode) {
    return mkdirat(AT_FDCWD, path, mode);
}

int mkdirat(int dirfd, const char* path, mode_t mode) {
    mxio_t* io = NULL;
    mx_status_t r;

    mode = (mode & 0777) | S_IFDIR;

    if ((r = __mxio_open_at(&io, dirfd, path, O_CREAT | O_EXCL | O_RDWR, mode)) < 0) {
        return ERROR(r);
    }
    io->ops->close(io);
    mxio_release(io);
    return 0;
}

int fsync(int fd) {
    mxio_t* io = fd_to_io(fd);
    if (io == NULL) {
        return ERRNO(EBADF);
    }
    int r = STATUS(io->ops->misc(io, MXRIO_SYNC, 0, 0, 0, 0));
    mxio_release(io);
    return r;
}

int fdatasync(int fd) {
    // TODO(smklein): fdatasync does not need to flush metadata under certain
    // circumstances -- however, for now, this implementation will appear
    // functionally the same (if a little slower).
    return fsync(fd);
}

int fstat(int fd, struct stat* s) {
    mxio_t* io = fd_to_io(fd);
    if (io == NULL) {
        return ERRNO(EBADF);
    }
    int r = STATUS(mxio_stat(io, s));
    mxio_release(io);
    return r;
}

int fstatat(int dirfd, const char* fn, struct stat* s, int flags) {
    mxio_t* io;
    mx_status_t r;

    if ((r = __mxio_open_at(&io, dirfd, fn, 0, 0)) < 0) {
        return ERROR(r);
    }
    r = mxio_stat(io, s);
    mxio_close(io);
    mxio_release(io);
    return STATUS(r);
}

int stat(const char* fn, struct stat* s) {
    return fstatat(AT_FDCWD, fn, s, 0);
}

static int mx_utimens(mxio_t* io, const struct timespec times[2], int flags) {
    vnattr_t vn;
    mx_status_t r;

    vn.valid = 0;

    // extract modify time
    vn.modify_time = (times == NULL || times[1].tv_nsec == UTIME_NOW)
        ? mx_time_get(MX_CLOCK_UTC)
        : MX_SEC(times[1].tv_sec) + times[1].tv_nsec;

    if (times == NULL || times[1].tv_nsec != UTIME_OMIT) {
        // TODO(orr) UTIME_NOW requires write access or euid == owner or "appropriate privilege"
        vn.valid = ATTR_MTIME;      // for setattr, tell which fields are valid
    }

    // TODO(orr): access time not implemented for now

    // set time(s) on underlying object
    r = mxio_setattr(io, &vn);
    return r;
}

int utimensat(int dirfd, const char *fn,
              const struct timespec times[2], int flags) {
    mxio_t* io;
    mx_status_t r;

    // TODO(orr): AT_SYMLINK_NOFOLLOW
    if ((flags & AT_SYMLINK_NOFOLLOW) != 0) {
        return ERRNO(EINVAL);
    }

    if ((r = __mxio_open_at(&io, dirfd, fn, 0, 0)) < 0) {
        return ERROR(r);
    }

    r = mx_utimens(io, times, 0);

    mxio_close(io);
    mxio_release(io);
    return STATUS(r);
}

int futimens(int fd, const struct timespec times[2]) {
    mxio_t* io = fd_to_io(fd);

    mx_status_t r = mx_utimens(io, times, 0);
    return STATUS(r);
}

int pipe2(int pipefd[2], int flags) {
    const int allowed_flags = O_NONBLOCK | O_CLOEXEC;
    if (flags & ~allowed_flags) {
        return ERRNO(EINVAL);
    }
    mxio_t *a, *b;
    int r = mxio_pipe_pair(&a, &b);
    if (r < 0) {
        return ERROR(r);
    }
    pipefd[0] = mxio_bind_to_fd(a, -1, 0);
    if (pipefd[0] < 0) {
        mxio_close(a);
        mxio_release(a);
        mxio_close(b);
        mxio_release(b);
        return ERROR(pipefd[0]);
    }
    pipefd[1] = mxio_bind_to_fd(b, -1, 0);
    if (pipefd[1] < 0) {
        close(pipefd[0]);
        mxio_close(b);
        mxio_release(b);
        return ERROR(pipefd[1]);
    }
    return 0;
}

int pipe(int pipefd[2]) {
    return pipe2(pipefd, 0);
}

int faccessat(int dirfd, const char* filename, int amode, int flag) {
    // For now, we just check to see if the file exists, until we
    // model permissions. But first, check that the flags and amode
    // are valid.
    const int allowed_flags = AT_EACCESS;
    if (flag & (~allowed_flags)) {
        return ERRNO(EINVAL);
    }

    // amode is allowed to be either a subset of this mask, or just F_OK.
    const int allowed_modes = R_OK | W_OK | X_OK;
    if (amode != F_OK && (amode & (~allowed_modes))) {
        return ERRNO(EINVAL);
    }

    // Since we are not tracking permissions yet, just check that the
    // file exists a la fstatat.
    mxio_t* io;
    mx_status_t status;
    if ((status = __mxio_open_at(&io, dirfd, filename, 0, 0)) < 0) {
        return ERROR(status);
    }
    struct stat s;
    status = mxio_stat(io, &s);
    mxio_close(io);
    mxio_release(io);
    return STATUS(status);
}

char* getcwd(char* buf, size_t size) {
    char tmp[PATH_MAX];
    if (buf == NULL) {
        buf = tmp;
        size = PATH_MAX;
    } else if (size == 0) {
        errno = EINVAL;
        return NULL;
    }

    char* out = NULL;
    mtx_lock(&mxio_cwd_lock);
    size_t len = strlen(mxio_cwd_path) + 1;
    if (len < size) {
        memcpy(buf, mxio_cwd_path, len);
        out = buf;
    } else {
        errno = ERANGE;
    }
    mtx_unlock(&mxio_cwd_lock);

    if (out == tmp) {
        out = strdup(tmp);
    }
    return out;
}

int chdir(const char* path) {
    mxio_t* io;
    mx_status_t r;
    if ((r = __mxio_open(&io, path, O_DIRECTORY, 0)) < 0) {
        return STATUS(r);
    }
    mtx_lock(&mxio_cwd_lock);
    update_cwd_path(path);
    mtx_lock(&mxio_lock);
    mxio_t* old = mxio_cwd_handle;
    mxio_cwd_handle = io;
    old->ops->close(old);
    mxio_release(old);
    mtx_unlock(&mxio_lock);
    mtx_unlock(&mxio_cwd_lock);
    return 0;
}

#define DIR_BUFSIZE 2048

struct __dirstream {
    mtx_t lock;
    int fd;
    // Total size of 'data' which has been filled with dirents
    size_t size;
    // Offset into 'data' of next ptr. NULL to reset the
    // directory lazily on the next call to getdirents
    uint8_t* ptr;
    // Internal cache of dirents
    uint8_t data[DIR_BUFSIZE];
    // Buffer returned to user
    struct dirent de;
};

static DIR* internal_opendir(int fd) {
    DIR* dir = calloc(1, sizeof(*dir));
    if (dir != NULL) {
        mtx_init(&dir->lock, mtx_plain);
        dir->size = 0;
        dir->fd = fd;
    }
    return dir;
}

DIR* opendir(const char* name) {
    int fd = open(name, O_DIRECTORY);
    if (fd < 0)
        return NULL;
    DIR* dir = internal_opendir(fd);
    if (dir == NULL)
        close(fd);
    return dir;
}

DIR* fdopendir(int fd) {
    // Check the fd for validity, but we'll just store the fd
    // number so we don't save the mxio_t pointer.
    mxio_t* io = fd_to_io(fd);
    if (io == NULL) {
        errno = EBADF;
        return NULL;
    }
    // TODO(mcgrathr): Technically this should verify that it's
    // really a directory and fail with ENOTDIR if not.  But
    // that's not so easy to do, so don't bother for now.
    mxio_release(io);
    return internal_opendir(fd);
}

int closedir(DIR* dir) {
    close(dir->fd);
    free(dir);
    return 0;
}

struct dirent* readdir(DIR* dir) {
    mtx_lock(&dir->lock);
    struct dirent* de = &dir->de;
    for (;;) {
        if (dir->size >= sizeof(vdirent_t)) {
            vdirent_t* vde = (void*)dir->ptr;
            if (dir->size >= vde->size) {
                de->d_ino = 0;
                de->d_off = 0;
                de->d_reclen = 0;
                de->d_type = vde->type;
                strcpy(de->d_name, vde->name);
                dir->ptr += vde->size;
                dir->size -= vde->size;
                break;
            }
            dir->size = 0;
        }
        int64_t cmd = (dir->ptr == NULL) ? READDIR_CMD_RESET : READDIR_CMD_NONE;
        int r = getdirents(dir->fd, dir->data, DIR_BUFSIZE, cmd);
        if (r > 0) {
            dir->ptr = dir->data;
            dir->size = r;
            continue;
        }
        de = NULL;
        break;
    }
    mtx_unlock(&dir->lock);
    return de;
}

void rewinddir(DIR* dir) {
    mtx_lock(&dir->lock);
    dir->size = 0;
    dir->ptr = NULL;
    mtx_unlock(&dir->lock);
}

int dirfd(DIR* dir) {
    return dir->fd;
}

int isatty(int fd) {
    mxio_t* io = fd_to_io(fd);
    if (io == NULL) {
        errno = EBADF;
        return 0;
    }

    int ret;
    // For now, stdout etc. needs to be a tty for line buffering to
    // work. So let's pretend those are ttys but nothing else is.
    if (fd == 0 || fd == 1 || fd == 2) {
        ret = 1;
    } else {
        ret = 0;
        errno = ENOTTY;
    }

    mxio_release(io);

    return ret;
}

mode_t umask(mode_t mask) {
    mode_t oldmask;
    mtx_lock(&mxio_lock);
    oldmask = __mxio_global_state.umask;
    __mxio_global_state.umask = mask & 0777;
    mtx_unlock(&mxio_lock);
    return oldmask;
}

int mxio_handle_fd(mx_handle_t h, mx_signals_t signals_in, mx_signals_t signals_out,
                   bool shared_handle) {
    mxio_t* io = mxio_waitable_create(h, signals_in, signals_out, shared_handle);
    int fd = mxio_bind_to_fd(io, -1, 0);
    if (fd < 0) {
        mxio_close(io);
        mxio_release(io);
    }
    return fd;
}

// from mxio/private.h, to support message-loop integration

void __mxio_wait_begin(mxio_t* io, uint32_t events,
                       mx_handle_t* handle_out, mx_signals_t* signals_out) {
    return io->ops->wait_begin(io, events, handle_out, signals_out);
}

void __mxio_wait_end(mxio_t* io, mx_signals_t signals, uint32_t* events_out) {
    return io->ops->wait_end(io, signals, events_out);
}

void __mxio_release(mxio_t* io) {
    mxio_release(io);
}


// TODO: getrlimit(RLIMIT_NOFILE, ...)
#define MAX_POLL_NFDS 1024

int poll(struct pollfd* fds, nfds_t n, int timeout) {
    if (n > MAX_POLL_NFDS) {
        return ERRNO(EINVAL);
    }

    mxio_t* ios[n];
    int ios_used_max = -1;

    mx_status_t r = NO_ERROR;
    nfds_t nvalid = 0;

    mx_wait_item_t items[n];

    for (nfds_t i = 0; i < n; i++) {
        struct pollfd* pfd = &fds[i];
        pfd->revents = 0; // initialize to zero

        ios[i] = NULL;
        if (pfd->fd < 0) {
            // if fd is negative, the entry is invalid
            continue;
        }
        mxio_t* io;
        if ((io = fd_to_io(pfd->fd)) == NULL) {
            // fd is not opened
            pfd->revents = POLLNVAL;
            continue;
        }
        ios[i] = io;
        ios_used_max = i;

        mx_handle_t h;
        mx_signals_t sigs;
        io->ops->wait_begin(io, pfd->events, &h, &sigs);
        if (h == MX_HANDLE_INVALID) {
            // wait operation is not applicable to the handle
            r = ERR_INVALID_ARGS;
            break;
        }
        items[nvalid].handle = h;
        items[nvalid].waitfor = sigs;
        items[nvalid].pending = 0;
        nvalid++;
    }

    int nfds = 0;
    if (r == NO_ERROR && nvalid > 0) {
        mx_time_t tmo = (timeout >= 0) ? MX_MSEC(timeout) : MX_TIME_INFINITE;
        r = mx_object_wait_many(items, nvalid, tmo);
        // pending signals could be reported on ERR_TIMED_OUT case as well
        if (r == NO_ERROR || r == ERR_TIMED_OUT) {
            nfds_t j = 0; // j counts up on a valid entry

            for (nfds_t i = 0; i < n; i++) {
                struct pollfd* pfd = &fds[i];
                mxio_t* io = ios[i];

                if (io == NULL) {
                    // skip an invalid entry
                    continue;
                }
                if (j < nvalid) {
                    uint32_t events = 0;
                    io->ops->wait_end(io, items[j].pending, &events);
                    // mask unrequested events except HUP/ERR
                    pfd->revents = events & (pfd->events | EPOLLHUP | EPOLLERR);
                    if (pfd->revents != 0) {
                        nfds++;
                    }
                }
                j++;
            }
        }
    }

    for (int i = 0; i <= ios_used_max; i++) {
        if (ios[i]) {
            mxio_release(ios[i]);
        }
    }

    return (r == NO_ERROR || r == ERR_TIMED_OUT) ? nfds : ERROR(r);
}

int select(int n, fd_set* restrict rfds, fd_set* restrict wfds, fd_set* restrict efds,
           struct timeval* restrict tv) {
    if (n > FD_SETSIZE || n < 1) {
        return ERRNO(EINVAL);
    }

    mxio_t* ios[n];
    int ios_used_max = -1;

    mx_status_t r = NO_ERROR;
    int nvalid = 0;

    mx_wait_item_t items[n];

    for (int fd = 0; fd < n; fd++) {
        ios[fd] = NULL;

        uint32_t events = 0;
        if (rfds && FD_ISSET(fd, rfds))
            events |= EPOLLIN;
        if (wfds && FD_ISSET(fd, wfds))
            events |= EPOLLOUT;
        if (efds && FD_ISSET(fd, efds))
            events |= EPOLLERR;
        if (events == 0) {
            continue;
        }

        mxio_t* io;
        if ((io = fd_to_io(fd)) == NULL) {
            r = ERR_BAD_HANDLE;
            break;
        }
        ios[fd] = io;
        ios_used_max = fd;

        mx_handle_t h;
        mx_signals_t sigs;
        io->ops->wait_begin(io, events, &h, &sigs);
        if (h == MX_HANDLE_INVALID) {
            r = ERR_INVALID_ARGS;
            break;
        }
        items[nvalid].handle = h;
        items[nvalid].waitfor = sigs;
        items[nvalid].pending = 0;
        nvalid++;
    }

    int nfds = 0;
    if (r == NO_ERROR && nvalid > 0) {
        mx_time_t tmo = (tv == NULL) ? MX_TIME_INFINITE :
            MX_SEC(tv->tv_sec) + MX_USEC(tv->tv_usec);
        r = mx_object_wait_many(items, nvalid, tmo);
        // pending signals could be reported on ERR_TIMED_OUT case as well
        if (r == NO_ERROR || r == ERR_TIMED_OUT) {
            int j = 0; // j counts up on a valid entry

            for (int fd = 0; fd < n; fd++) {
                mxio_t* io = ios[fd];
                if (io == NULL) {
                    // skip an invalid entry
                    continue;
                }
                if (j < nvalid) {
                    uint32_t events = 0;
                    io->ops->wait_end(io, items[j].pending, &events);
                    if (rfds && FD_ISSET(fd, rfds)) {
                        if (events & EPOLLIN) {
                            nfds++;
                        } else {
                            FD_CLR(fd, rfds);
                        }
                    }
                    if (wfds && FD_ISSET(fd, wfds)) {
                        if (events & EPOLLOUT) {
                            nfds++;
                        } else {
                            FD_CLR(fd, wfds);
                        }
                    }
                    if (efds && FD_ISSET(fd, efds)) {
                        if (events & EPOLLERR) {
                            nfds++;
                        } else {
                            FD_CLR(fd, efds);
                        }
                    }
                } else {
                    if (rfds) {
                        FD_CLR(fd, rfds);
                    }
                    if (wfds) {
                        FD_CLR(fd, wfds);
                    }
                    if (efds) {
                        FD_CLR(fd, efds);
                    }
                }
                j++;
            }
        }
    }

    for (int i = 0; i <= ios_used_max; i++) {
        if (ios[i]) {
            mxio_release(ios[i]);
        }
    }

    return (r == NO_ERROR || r == ERR_TIMED_OUT) ? nfds : ERROR(r);
}

int ioctl(int fd, int req, ...) {
    mxio_t* io;
    if ((io = fd_to_io(fd)) == NULL) {
        return ERRNO(EBADF);
    }
    va_list ap;
    va_start(ap, req);
    ssize_t r = io->ops->posix_ioctl(io, req, ap);
    va_end(ap);
    mxio_release(io);
    return STATUS(r);
}
