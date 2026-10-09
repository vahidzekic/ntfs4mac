/*
 * ntfsb_devio.c — libntfs-3g device operations backed by ntfsb_io callbacks.
 *
 * See ntfsb_devio.h for the rationale. Everything here runs with the
 * per-volume bridge mutex held (or the global format mutex for mkntfs), so
 * the private state needs no locking of its own.
 *
 * libntfs-3g is GPL-2.0-or-later; anything linking this file inherits that.
 */

#include "ntfsb_devio.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>  /* libntfs-3g headers need it without config.h */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/fs.h>     /* BLKGETSIZE64, BLKGETSIZE, BLKSSZGET, BLKBSZSET */
#endif
#if defined(__APPLE__)
#include <sys/disk.h>     /* DKIOCGETBLOCKCOUNT, DKIOCGETBLOCKSIZE */
#endif

#include <ntfs-3g/types.h>
#include <ntfs-3g/device.h>

struct ntfsb_devio {
    ntfsb_io io;             /* copy of the host callbacks */
    int64_t pos;             /* file position for seek/read/write */
    uint8_t *bounce;         /* one logical block, for unaligned edges */
    uint64_t written;        /* running total reported to the observer */
    ntfsb_write_observer observer;
    void *observer_ctx;
    int observer_err;        /* sticky error returned by the observer */
};

static ntfsb_devio *devio_of(struct ntfs_device *dev)
{
    return (ntfsb_devio *)dev->d_private;
}

ntfsb_devio *ntfsb_devio_new(const ntfsb_io *io)
{
    if (!io || !io->pread || io->block_size == 0 ||
        (io->block_size & (io->block_size - 1)) != 0 ||
        (!io->read_only && !io->pwrite)) {
        errno = EINVAL;
        return NULL;
    }
    ntfsb_devio *dio = calloc(1, sizeof(*dio));
    if (!dio) {
        errno = ENOMEM;
        return NULL;
    }
    dio->bounce = malloc(io->block_size);
    if (!dio->bounce) {
        free(dio);
        errno = ENOMEM;
        return NULL;
    }
    dio->io = *io;
    return dio;
}

void ntfsb_devio_free(ntfsb_devio *dio)
{
    if (!dio)
        return;
    free(dio->bounce);
    free(dio);
}

/*
 * Aligned transfer through the host callbacks. Returns bytes transferred
 * (short only at end of device) or -1 with errno set. The callback contract
 * says negative return values are negated errnos.
 */
static int64_t raw_read(ntfsb_devio *dio, void *buf, uint64_t count, int64_t offset)
{
    int64_t r = dio->io.pread(dio->io.ctx, buf, count, offset);
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

static int64_t raw_write(ntfsb_devio *dio, const void *buf, uint64_t count, int64_t offset)
{
    int64_t r = dio->io.pwrite(dio->io.ctx, buf, count, offset);
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

/* Clamp a request to the device size. Returns the usable byte count. */
static int64_t clamp_to_device(const ntfsb_devio *dio, int64_t count, int64_t offset)
{
    int64_t size = (int64_t)dio->io.size_bytes;
    if (offset >= size)
        return 0;
    if (count > size - offset)
        count = size - offset;
    return count;
}

int64_t ntfsb_devio_pread(ntfsb_devio *dio, void *buf, int64_t count, int64_t offset)
{
    if (!dio || !buf || count < 0 || offset < 0) {
        errno = EINVAL;
        return -1;
    }
    count = clamp_to_device(dio, count, offset);

    const int64_t bs = dio->io.block_size;
    uint8_t *out = buf;
    int64_t done = 0;

    while (done < count) {
        int64_t off = offset + done;
        int64_t in_block = off % bs;
        int64_t remaining = count - done;

        if (in_block == 0 && remaining >= bs) {
            /* Aligned middle: straight into the caller's buffer. */
            int64_t chunk = remaining - remaining % bs;
            int64_t r = raw_read(dio, out + done, (uint64_t)chunk, off);
            if (r < 0)
                return done ? done : -1;
            done += r;
            if (r < chunk)
                break; /* end of device */
            continue;
        }

        /* Unaligned edge: bounce one whole block. */
        int64_t block_start = off - in_block;
        int64_t r = raw_read(dio, dio->bounce, (uint64_t)bs, block_start);
        if (r < 0)
            return done ? done : -1;
        if (r <= in_block)
            break; /* end of device inside this block */
        int64_t n = r - in_block;
        if (n > remaining)
            n = remaining;
        memcpy(out + done, dio->bounce + in_block, (size_t)n);
        done += n;
        if (r < bs)
            break;
    }
    return done;
}

/* Report progress to the observer; a non-zero reply poisons the device. */
static int notify_written(ntfsb_devio *dio, int64_t n)
{
    dio->written += (uint64_t)n;
    if (dio->observer && !dio->observer_err)
        dio->observer_err = dio->observer(dio->observer_ctx, dio->written);
    return dio->observer_err;
}

static int64_t devio_pwrite(ntfsb_devio *dio, const void *buf, int64_t count, int64_t offset)
{
    if (dio->io.read_only || !dio->io.pwrite) {
        errno = EROFS;
        return -1;
    }
    if (dio->observer_err) {
        errno = dio->observer_err;
        return -1;
    }
    if (count > 0 && clamp_to_device(dio, count, offset) < count) {
        /* NTFS never writes past the end of its own volume; refuse rather
         * than silently truncating metadata. */
        errno = ENOSPC;
        return -1;
    }

    const int64_t bs = dio->io.block_size;
    const uint8_t *in = buf;
    int64_t done = 0;

    while (done < count) {
        int64_t off = offset + done;
        int64_t in_block = off % bs;
        int64_t remaining = count - done;
        int64_t n;

        if (in_block == 0 && remaining >= bs) {
            int64_t chunk = remaining - remaining % bs;
            n = raw_write(dio, in + done, (uint64_t)chunk, off);
            if (n < 0)
                return done ? done : -1;
            if (n < chunk) {
                done += n;
                break;
            }
        } else {
            /*
             * Read-modify-write of a partial block. A failed or short read
             * must abort: writing back a partly stale bounce buffer would
             * corrupt the neighbouring bytes on disk.
             */
            int64_t block_start = off - in_block;
            int64_t r = raw_read(dio, dio->bounce, (uint64_t)bs, block_start);
            if (r < 0)
                return done ? done : -1;
            if (r < bs) {
                errno = EIO;
                return done ? done : -1;
            }
            n = bs - in_block;
            if (n > remaining)
                n = remaining;
            memcpy(dio->bounce + in_block, in + done, (size_t)n);
            int64_t w = raw_write(dio, dio->bounce, (uint64_t)bs, block_start);
            if (w < 0)
                return done ? done : -1;
            if (w < bs) {
                errno = EIO;
                return done ? done : -1;
            }
        }
        done += n;
        if (notify_written(dio, n)) {
            /* Data of this chunk is on disk; report what we did, the next
             * call fails with the observer's error. */
            break;
        }
    }
    return done;
}

/* ------------------------------------------------------------------------ */
/* struct ntfs_device_operations                                             */
/* ------------------------------------------------------------------------ */

static int op_open(struct ntfs_device *dev, int flags)
{
    ntfsb_devio *dio = devio_of(dev);
    if (!dio) {
        errno = ENODEV;
        return -1;
    }
    if (NDevOpen(dev)) {
        errno = EBUSY;
        return -1;
    }
    /*
     * libntfs-3g first tries O_RDWR and falls back to O_RDONLY (marking the
     * volume read-only) when open fails with EROFS.
     */
    if ((flags & O_ACCMODE) != O_RDONLY && dio->io.read_only) {
        errno = EROFS;
        return -1;
    }
    if ((flags & O_ACCMODE) == O_RDONLY)
        NDevSetReadOnly(dev);
    else
        NDevClearReadOnly(dev);
    /* Pretend to be a block device: mkntfs refuses plain files without -F
     * and libntfs-3g only uses the size ioctls for block devices. */
    NDevSetBlock(dev);
    dio->pos = 0;
    NDevSetOpen(dev);
    return 0;
}

static int op_sync(struct ntfs_device *dev)
{
    ntfsb_devio *dio = devio_of(dev);
    if (!NDevReadOnly(dev) && dio->io.sync) {
        int err = dio->io.sync(dio->io.ctx);
        if (err) {
            errno = err;
            return -1;
        }
    }
    NDevClearDirty(dev);
    return 0;
}

static int op_close(struct ntfs_device *dev)
{
    if (!NDevOpen(dev)) {
        errno = EBADF;
        return -1;
    }
    int rc = 0;
    if (NDevDirty(dev))
        rc = op_sync(dev);
    NDevClearOpen(dev);
    return rc;
}

static s64 op_seek(struct ntfs_device *dev, s64 offset, int whence)
{
    ntfsb_devio *dio = devio_of(dev);
    int64_t base;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = dio->pos; break;
    case SEEK_END: base = (int64_t)dio->io.size_bytes; break;
    default:
        errno = EINVAL;
        return -1;
    }
    if ((offset > 0 && base > INT64_MAX - offset) || base + offset < 0) {
        errno = EINVAL;
        return -1;
    }
    dio->pos = base + offset;
    return dio->pos;
}

static s64 op_pread(struct ntfs_device *dev, void *buf, s64 count, s64 offset)
{
    return ntfsb_devio_pread(devio_of(dev), buf, count, offset);
}

static s64 op_pwrite(struct ntfs_device *dev, const void *buf, s64 count, s64 offset)
{
    if (!buf || count < 0 || offset < 0) {
        errno = EINVAL;
        return -1;
    }
    if (NDevReadOnly(dev)) {
        errno = EROFS;
        return -1;
    }
    s64 r = devio_pwrite(devio_of(dev), buf, count, offset);
    if (r > 0)
        NDevSetDirty(dev);
    return r;
}

static s64 op_read(struct ntfs_device *dev, void *buf, s64 count)
{
    ntfsb_devio *dio = devio_of(dev);
    s64 r = op_pread(dev, buf, count, dio->pos);
    if (r > 0)
        dio->pos += r;
    return r;
}

static s64 op_write(struct ntfs_device *dev, const void *buf, s64 count)
{
    ntfsb_devio *dio = devio_of(dev);
    s64 r = op_pwrite(dev, buf, count, dio->pos);
    if (r > 0)
        dio->pos += r;
    return r;
}

static int op_stat(struct ntfs_device *dev, struct stat *st)
{
    ntfsb_devio *dio = devio_of(dev);
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFBLK | (dio->io.read_only ? 0400 : 0600);
    st->st_size = (off_t)dio->io.size_bytes;
    st->st_blksize = (blksize_t)dio->io.block_size;
    st->st_blocks = (blkcnt_t)(dio->io.size_bytes / 512);
    return 0;
}

/*
 * libntfs-3g (device.c) asks for the device geometry through platform
 * ioctls: BLKGETSIZE64/BLKGETSIZE/BLKSSZGET/BLKBSZSET on Linux and
 * DKIOCGETBLOCKCOUNT/DKIOCGETBLOCKSIZE on macOS. We answer the ones that
 * the platform's libntfs-3g build compiled in, from the ntfsb_io
 * description. Everything else (HDIO_GETGEO, BLKDISCARD, ...) is ENOTTY;
 * libntfs-3g treats that as "unknown" and mkntfs always gets explicit
 * geometry on its command line.
 */
static int op_ioctl(struct ntfs_device *dev, unsigned long request, void *argp)
{
    ntfsb_devio *dio = devio_of(dev);
    (void)argp;
    switch (request) {
#if defined(BLKGETSIZE64)
    case BLKGETSIZE64:
        *(uint64_t *)argp = dio->io.size_bytes;
        return 0;
#endif
#if defined(BLKGETSIZE)
    case BLKGETSIZE:
        *(unsigned long *)argp = (unsigned long)(dio->io.size_bytes / 512);
        return 0;
#endif
#if defined(BLKSSZGET)
    case BLKSSZGET:
        *(int *)argp = (int)dio->io.block_size;
        return 0;
#endif
#if defined(BLKBSZSET)
    case BLKBSZSET:
        /* Any block size works: unaligned IO is bounced here. */
        return 0;
#endif
#if defined(DKIOCGETBLOCKCOUNT)
    case DKIOCGETBLOCKCOUNT:
        *(uint64_t *)argp = dio->io.size_bytes / dio->io.block_size;
        return 0;
#endif
#if defined(DKIOCGETBLOCKSIZE)
    case DKIOCGETBLOCKSIZE:
        *(uint32_t *)argp = dio->io.block_size;
        return 0;
#endif
    default:
        errno = ENOTTY;
        return -1;
    }
}

struct ntfs_device_operations ntfsb_device_ops = {
    .open = op_open,
    .close = op_close,
    .seek = op_seek,
    .read = op_read,
    .write = op_write,
    .pread = op_pread,
    .pwrite = op_pwrite,
    .sync = op_sync,
    .stat = op_stat,
    .ioctl = op_ioctl,
};

/* ------------------------------------------------------------------------ */
/* mkntfs binding                                                            */
/* ------------------------------------------------------------------------ */

static struct {
    const ntfsb_io *io;
    ntfsb_write_observer observer;
    void *observer_ctx;
} g_mkntfs_binding;

void ntfsb_mkntfs_bind(const ntfsb_io *io, ntfsb_write_observer observer, void *observer_ctx)
{
    g_mkntfs_binding.io = io;
    g_mkntfs_binding.observer = io ? observer : NULL;
    g_mkntfs_binding.observer_ctx = io ? observer_ctx : NULL;
}

static int mkntfs_op_open(struct ntfs_device *dev, int flags)
{
    if (!g_mkntfs_binding.io) {
        /* mkntfs code reached without ntfsb_format(): there is no device. */
        errno = ENODEV;
        return -1;
    }
    if (dev->d_private) {
        errno = EBUSY;
        return -1;
    }
    /* mkntfs' main() has installed its stdout/stderr log handler by now. */
    ntfsb_log_install_mkntfs();
    ntfsb_devio *dio = ntfsb_devio_new(g_mkntfs_binding.io);
    if (!dio)
        return -1;
    dio->observer = g_mkntfs_binding.observer;
    dio->observer_ctx = g_mkntfs_binding.observer_ctx;
    dev->d_private = dio;
    if (op_open(dev, flags)) {
        int err = errno;
        dev->d_private = NULL;
        ntfsb_devio_free(dio);
        errno = err;
        return -1;
    }
    return 0;
}

static int mkntfs_op_close(struct ntfs_device *dev)
{
    int rc = op_close(dev);
    int err = errno;
    ntfsb_devio_free(devio_of(dev));
    dev->d_private = NULL;
    errno = err;
    return rc;
}

struct ntfs_device_operations ntfsb_mkntfs_io_ops = {
    .open = mkntfs_op_open,
    .close = mkntfs_op_close,
    .seek = op_seek,
    .read = op_read,
    .write = op_write,
    .pread = op_pread,
    .pwrite = op_pwrite,
    .sync = op_sync,
    .stat = op_stat,
    .ioctl = op_ioctl,
};
