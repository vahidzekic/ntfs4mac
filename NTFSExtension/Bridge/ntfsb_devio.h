/*
 * ntfsb_devio.h — libntfs-3g device operations backed by ntfsb_io callbacks.
 *
 * libntfs-3g performs all sector IO through a `struct ntfs_device_operations`
 * table. The stock table (unix_io.c) opens a path with open(2); inside the
 * FSKit sandbox we never get a path, only an FSBlockDeviceResource, so this
 * module provides a table that forwards every request to the host's
 * ntfsb_io callbacks instead.
 *
 * libntfs-3g issues requests of arbitrary size and alignment (boot sector
 * reads of 512 bytes, MFT records of 1024 bytes, single-byte bitmap
 * updates, ...). FSKit block devices only accept whole logical blocks, so
 * this layer splits every request into an aligned middle part that goes
 * straight to the callbacks and at most two partial edge blocks that are
 * bounced through a private buffer (read-modify-write for writes).
 *
 * Internal to the bridge; not part of the frozen NTFSBridge.h contract.
 *
 * libntfs-3g is GPL-2.0-or-later; anything linking this file inherits that.
 */

#ifndef NTFSB_DEVIO_H
#define NTFSB_DEVIO_H

#include <stdint.h>

#include "NTFSBridge.h"

struct ntfs_device;
struct ntfs_device_operations;

/* Private state hung off ntfs_device.d_private. Opaque outside devio.c. */
typedef struct ntfsb_devio ntfsb_devio;

/*
 * Optional observer invoked after every successful write with the running
 * total of bytes written through this device. Returning a non-zero errno
 * makes the write (and all later writes) fail with that error; ntfsb_format
 * uses this for progress reporting and cancellation.
 */
typedef int (*ntfsb_write_observer)(void *ctx, uint64_t total_written);

/*
 * Operations table for volumes mounted by the bridge. The device's
 * d_private must be an ntfsb_devio created with ntfsb_devio_new(); the
 * bridge owns it and frees it after ntfs_umount()/ntfs_device_free().
 */
extern struct ntfs_device_operations ntfsb_device_ops;

/*
 * Operations table used by the in-process mkntfs. mkntfs.c is compiled so
 * that its reference to the default IO table resolves to this symbol (see
 * mkntfs_glue.h). mkntfs allocates the device with d_private == NULL; open()
 * attaches an ntfsb_devio built from the binding installed with
 * ntfsb_mkntfs_bind() and close() releases it again.
 */
extern struct ntfs_device_operations ntfsb_mkntfs_io_ops;

/* Allocate private device state. Returns NULL (errno = ENOMEM/EINVAL) on failure. */
ntfsb_devio *ntfsb_devio_new(const ntfsb_io *io);
void ntfsb_devio_free(ntfsb_devio *dio);

/*
 * Positioned IO on the private state directly (used for probing the boot
 * sector before libntfs-3g is involved). Same contract as the ntfs device
 * ops: return bytes transferred or -1 with errno set.
 */
int64_t ntfsb_devio_pread(ntfsb_devio *dio, void *buf, int64_t count, int64_t offset);

/*
 * Install (io != NULL) or clear (io == NULL) the process-global binding that
 * ntfsb_mkntfs_io_ops.open() picks up. `io` must stay valid until cleared.
 * The caller serialises formats (ntfsb_format holds a global mutex), so the
 * binding is never replaced while mkntfs is running.
 */
void ntfsb_mkntfs_bind(const ntfsb_io *io, ntfsb_write_observer observer, void *observer_ctx);

#endif /* NTFSB_DEVIO_H */
