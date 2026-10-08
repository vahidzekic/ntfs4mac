/*
 * NTFSBridge.h — thin, Swift-friendly C facade over libntfs-3g.
 *
 * Design rules (see docs/ARCHITECTURE.md):
 *  - Every function returns 0 on success or a positive POSIX errno value on
 *    failure. No function ever reports failure through the global `errno`.
 *  - Files are addressed by NTFS inode number (MFT record number, the low
 *    48 bits of the MFT reference). The root directory is NTFSB_ROOT_INO.
 *  - Strings crossing the boundary are NUL-terminated UTF-8. Conversion to
 *    and from NTFS UTF-16LE happens inside the bridge.
 *  - libntfs-3g is not thread safe. Every call that takes an ntfsb_volume
 *    serialises on a per-volume mutex owned by the bridge, so Swift may call
 *    in from any thread or task.
 *  - The bridge never opens /dev nodes itself. All sector IO goes through
 *    the ntfsb_io callbacks, which FSKit implements on top of
 *    FSBlockDeviceResource. The bridge guarantees that every callback
 *    request is aligned to ntfsb_io.block_size in both offset and length
 *    (it performs read-modify-write bouncing internally).
 *
 * libntfs-3g is GPL-2.0-or-later; anything linking this bridge inherits that.
 */

#ifndef NTFS_BRIDGE_H
#define NTFS_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MFT record number of the root directory ("." of the volume). */
#define NTFSB_ROOT_INO 5ULL

/* First MFT record usable by ordinary files; 0..15 are NTFS metadata. */
#define NTFSB_FIRST_USER_INO 16ULL

#define NTFSB_MAX_NAME_UTF8 1024 /* 255 UTF-16 units → ≤ 765 UTF-8 bytes */
#define NTFSB_MAX_LABEL_UTF8 128 /* 32 UTF-16 units  → ≤ 96 UTF-8 bytes  */

/* ------------------------------------------------------------------------ */
/* Block IO callbacks supplied by the host (Swift / FSKit).                 */
/* ------------------------------------------------------------------------ */

/*
 * All callbacks receive `ctx` verbatim. `offset` and `count` are always
 * multiples of `block_size`. Return the number of bytes transferred
 * (which must equal `count` unless EOF was hit) or a negative errno.
 */
typedef int64_t (*ntfsb_pread_fn)(void *ctx, void *buf, uint64_t count, int64_t offset);
typedef int64_t (*ntfsb_pwrite_fn)(void *ctx, const void *buf, uint64_t count, int64_t offset);
/* Flush the device's write cache. Return 0 or a positive errno. */
typedef int (*ntfsb_sync_fn)(void *ctx);

typedef struct ntfsb_io {
    void *ctx;
    ntfsb_pread_fn pread;
    ntfsb_pwrite_fn pwrite;   /* may be NULL when read_only is true */
    ntfsb_sync_fn sync;       /* may be NULL */
    uint64_t size_bytes;      /* total size of the partition/device in bytes */
    uint32_t block_size;      /* logical sector size the device accepts (512 or 4096) */
    bool read_only;
} ntfsb_io;

/* ------------------------------------------------------------------------ */
/* Probing                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct ntfsb_probe_info {
    bool is_ntfs;                         /* boot sector is a valid NTFS boot sector */
    char label[NTFSB_MAX_LABEL_UTF8];     /* volume label, "" if none or not readable */
    uint64_t serial;                      /* 64-bit volume serial number from boot sector */
    uint8_t uuid[16];                     /* stable UUID derived from serial (RFC 4122 v5-like, deterministic) */
    uint32_t bytes_per_sector;
    uint32_t cluster_size;
    uint64_t total_sectors;
    bool dirty;                           /* $Volume dirty flag set (needs chkdsk) */
    bool hibernated;                      /* hiberfil.sys / fast-startup detected */
} ntfsb_probe_info;

/*
 * Read-only probe. Reads the boot sector, and if it is NTFS tries a
 * read-only mount to fetch the label/dirty/hibernation state. Returns 0 and
 * is_ntfs=false for a non-NTFS device (that is not an error).
 */
int ntfsb_probe(const ntfsb_io *io, ntfsb_probe_info *out);

/* ------------------------------------------------------------------------ */
/* Mounting                                                                  */
/* ------------------------------------------------------------------------ */

typedef struct ntfsb_volume ntfsb_volume; /* opaque */

enum {
    NTFSB_MOUNT_RDONLY          = 1u << 0,
    NTFSB_MOUNT_REMOVE_HIBER    = 1u << 1, /* delete hiberfil.sys if Windows is hibernated */
    NTFSB_MOUNT_RECOVER         = 1u << 2, /* reset the journal ($LogFile) when unclean */
    NTFSB_MOUNT_SHOW_SYS_FILES  = 1u << 3, /* expose $MFT, $Bitmap, ... in the root dir */
    NTFSB_MOUNT_IGNORE_CASE     = 1u << 4, /* case-insensitive lookups */
};

typedef struct ntfsb_mount_opts {
    uint32_t flags;      /* NTFSB_MOUNT_* */
    uint32_t uid;        /* owner reported for every item */
    uint32_t gid;        /* group reported for every item */
    uint32_t fmask;      /* permission bits cleared from files (e.g. 0022) */
    uint32_t dmask;      /* permission bits cleared from directories */
} ntfsb_mount_opts;

/*
 * Mount the volume. `io` is copied; io->ctx must stay valid until
 * ntfsb_unmount() returns. On EPERM the volume is hibernated/unclean and the
 * caller may retry with RDONLY or REMOVE_HIBER/RECOVER.
 */
int ntfsb_mount(const ntfsb_io *io, const ntfsb_mount_opts *opts, ntfsb_volume **out_vol);

/* Flush and unmount. `force` unmounts even if inodes are still open. Frees `vol`. */
int ntfsb_unmount(ntfsb_volume *vol, bool force);

/* Flush all dirty metadata and data, then call io->sync. */
int ntfsb_sync(ntfsb_volume *vol);

typedef struct ntfsb_volume_info {
    char label[NTFSB_MAX_LABEL_UTF8];
    uint64_t serial;
    uint8_t uuid[16];               /* same derivation as ntfsb_probe_info.uuid */
    uint32_t cluster_size;          /* bytes */
    uint32_t sector_size;           /* bytes */
    uint64_t total_clusters;
    uint64_t free_clusters;
    uint64_t total_mft_records;     /* upper bound for file count */
    uint64_t free_mft_records;
    uint8_t major_ver;
    uint8_t minor_ver;
    bool read_only;
} ntfsb_volume_info;

int ntfsb_volume_info_get(ntfsb_volume *vol, ntfsb_volume_info *out);
int ntfsb_set_label(ntfsb_volume *vol, const char *label_utf8);

/* ------------------------------------------------------------------------ */
/* Attributes                                                                */
/* ------------------------------------------------------------------------ */

typedef enum ntfsb_type {
    NTFSB_TYPE_UNKNOWN = 0,
    NTFSB_TYPE_FILE    = 1,
    NTFSB_TYPE_DIR     = 2,
    NTFSB_TYPE_SYMLINK = 3, /* reparse point symlink / junction / WSL symlink */
} ntfsb_type;

/* Subset of NTFS FILE_ATTR_* that maps to BSD st_flags on macOS. */
enum {
    NTFSB_FLAG_READONLY = 1u << 0, /* FILE_ATTR_READONLY  → UF_IMMUTABLE */
    NTFSB_FLAG_HIDDEN   = 1u << 1, /* FILE_ATTR_HIDDEN    → UF_HIDDEN    */
    NTFSB_FLAG_SYSTEM   = 1u << 2, /* FILE_ATTR_SYSTEM                    */
    NTFSB_FLAG_ARCHIVE  = 1u << 3, /* FILE_ATTR_ARCHIVE                   */
    NTFSB_FLAG_COMPRESSED = 1u << 4,
    NTFSB_FLAG_ENCRYPTED  = 1u << 5,
    NTFSB_FLAG_SPARSE     = 1u << 6,
};

typedef struct ntfsb_stat {
    uint64_t ino;               /* MFT record number */
    uint64_t parent_ino;        /* MFT record number of (first) parent dir, 0 if unknown */
    ntfsb_type type;
    uint32_t mode;              /* full POSIX mode incl. S_IFMT bits */
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;              /* logical size in bytes (symlink: target length) */
    uint64_t alloc_size;        /* bytes allocated on disk */
    struct timespec atime;      /* last access */
    struct timespec mtime;      /* last data modification */
    struct timespec ctime;      /* last MFT record change */
    struct timespec btime;      /* creation (birth) */
    uint32_t flags;             /* NTFSB_FLAG_* */
    uint64_t generation;        /* MFT sequence number */
} ntfsb_stat;

int ntfsb_getattr(ntfsb_volume *vol, uint64_t ino, ntfsb_stat *out);

enum {
    NTFSB_SET_SIZE  = 1u << 0,
    NTFSB_SET_MODE  = 1u << 1, /* only the write bit is honoured → READONLY flag */
    NTFSB_SET_UID   = 1u << 2, /* accepted and ignored (no ownership on NTFS without mapping) */
    NTFSB_SET_GID   = 1u << 3, /* accepted and ignored */
    NTFSB_SET_ATIME = 1u << 4,
    NTFSB_SET_MTIME = 1u << 5,
    NTFSB_SET_CTIME = 1u << 6,
    NTFSB_SET_BTIME = 1u << 7,
    NTFSB_SET_FLAGS = 1u << 8, /* READONLY / HIDDEN / SYSTEM / ARCHIVE only */
};

typedef struct ntfsb_setattr_req {
    uint32_t mask;              /* NTFSB_SET_* */
    uint64_t size;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    struct timespec atime;
    struct timespec mtime;
    struct timespec ctime;
    struct timespec btime;
    uint32_t flags;
} ntfsb_setattr_req;

/* Apply `req` and return the resulting attributes in `out` (may be NULL). */
int ntfsb_setattr(ntfsb_volume *vol, uint64_t ino, const ntfsb_setattr_req *req, ntfsb_stat *out);

/* ------------------------------------------------------------------------ */
/* Namespace                                                                 */
/* ------------------------------------------------------------------------ */

/* Look up `name` in directory `dir_ino`. ENOENT if absent, ENOTDIR if dir_ino isn't a directory. */
int ntfsb_lookup(ntfsb_volume *vol, uint64_t dir_ino, const char *name_utf8, ntfsb_stat *out);

/*
 * Directory enumeration callback. Return 0 to continue, non-zero to stop
 * (e.g. the FSKit packer is full). `next_cookie` is the value to pass back
 * to ntfsb_readdir() to resume *after* this entry. "." and ".." are
 * reported (with cookies 1 and 2); DOS 8.3 aliases are never reported;
 * NTFS metadata files ($MFT, ...) are hidden unless SHOW_SYS_FILES.
 */
typedef int (*ntfsb_dirent_fn)(void *ctx, const char *name_utf8, uint64_t ino,
                               ntfsb_type type, uint64_t next_cookie);

/*
 * Enumerate `dir_ino` starting at `cookie` (0 = beginning). Returns 0 when
 * the end is reached or the callback stopped iteration; *out_eof (may be
 * NULL) tells which. EINVAL for a stale/invalid cookie.
 */
int ntfsb_readdir(ntfsb_volume *vol, uint64_t dir_ino, uint64_t cookie,
                  ntfsb_dirent_fn fn, void *ctx, bool *out_eof);

/* Create a regular file or directory. `mode` permission bits only. */
int ntfsb_create(ntfsb_volume *vol, uint64_t dir_ino, const char *name_utf8,
                 ntfsb_type type, uint32_t mode, ntfsb_stat *out);

/* Create a symbolic link (stored as an NTFS reparse point, Windows-compatible). */
int ntfsb_symlink(ntfsb_volume *vol, uint64_t dir_ino, const char *name_utf8,
                  const char *target_utf8, ntfsb_stat *out);

/* Read a symlink target into buf (NUL-terminated). ERANGE if too small. */
int ntfsb_readlink(ntfsb_volume *vol, uint64_t ino, char *buf, size_t bufsize, size_t *out_len);

/* Create a hard link `name` in `dir_ino` pointing at `ino`. */
int ntfsb_link(ntfsb_volume *vol, uint64_t ino, uint64_t dir_ino, const char *name_utf8);

/* Remove `name` from `dir_ino`. Works for files, symlinks and empty dirs (ENOTEMPTY otherwise). */
int ntfsb_unlink(ntfsb_volume *vol, uint64_t dir_ino, const char *name_utf8);

/*
 * Rename. If the destination exists it is replaced (POSIX semantics; a
 * non-empty destination directory yields ENOTEMPTY). Renaming a directory
 * into its own subtree yields EINVAL.
 */
int ntfsb_rename(ntfsb_volume *vol, uint64_t src_dir_ino, const char *src_name_utf8,
                 uint64_t dst_dir_ino, const char *dst_name_utf8);

/* ------------------------------------------------------------------------ */
/* File data                                                                 */
/* ------------------------------------------------------------------------ */

/* Read up to `len` bytes from the unnamed $DATA stream. Short read at EOF. */
int ntfsb_read(ntfsb_volume *vol, uint64_t ino, void *buf, uint64_t len,
               int64_t offset, uint64_t *out_read);

/* Write `len` bytes, extending the file as needed. */
int ntfsb_write(ntfsb_volume *vol, uint64_t ino, const void *buf, uint64_t len,
                int64_t offset, uint64_t *out_written);

/* Set the unnamed $DATA stream length. */
int ntfsb_truncate(ntfsb_volume *vol, uint64_t ino, uint64_t size);

/* ------------------------------------------------------------------------ */
/* Formatting (mkntfs)                                                       */
/* ------------------------------------------------------------------------ */

typedef struct ntfsb_format_opts {
    const char *label_utf8;      /* NULL or "" for no label */
    uint32_t cluster_size;       /* 0 = mkntfs default */
    bool quick;                  /* -Q: don't zero the volume */
    bool enable_compression;     /* -C */
    uint64_t hidden_sectors;     /* -p: partition start in sectors (for Windows boot), 0 ok */
} ntfsb_format_opts;

/*
 * Progress callback: percent in [0,100]. Return non-zero to request
 * cancellation (best effort; honoured between mkntfs phases).
 */
typedef int (*ntfsb_progress_fn)(void *ctx, double percent);

/*
 * Create a fresh NTFS file system on the device described by `io`
 * (io->read_only must be false). Runs the mkntfs code compiled into the
 * bridge with its device IO redirected to `io`. Calls are serialised
 * process-wide because mkntfs uses global state.
 */
int ntfsb_format(const ntfsb_io *io, const ntfsb_format_opts *opts,
                 ntfsb_progress_fn progress, void *progress_ctx);

/* Human-readable description of a bridge errno (static storage). */
const char *ntfsb_strerror(int err);

/* libntfs-3g version string, e.g. "2022.10.3". */
const char *ntfsb_libntfs_version(void);

#ifdef __cplusplus
}
#endif

#endif /* NTFS_BRIDGE_H */
