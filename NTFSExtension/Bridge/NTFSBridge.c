/*
 * NTFSBridge.c — implementation of NTFSBridge.h over libntfs-3g.
 *
 * Conventions used throughout:
 *  - Every public entry point that takes an ntfsb_volume holds v->lock for
 *    its whole duration; libntfs-3g is not thread safe.
 *  - No ntfs_inode stays open between calls. Every path opens the inodes it
 *    needs and closes them before returning (ntfs_inode_close() is also
 *    where libntfs-3g writes dirty MFT records and index entries back).
 *  - An inode is never opened twice at the same time: libntfs-3g keeps one
 *    in-memory copy of the MFT record per ntfs_inode, so two handles to the
 *    same record would overwrite each other's changes on close.
 *  - libntfs-3g reports errors via errno; we capture it right after the
 *    failing call and return it as a positive value.
 *
 * libntfs-3g is GPL-2.0-or-later; anything linking this file inherits that.
 */

#include "NTFSBridge.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <ntfs-3g/types.h>
#include <ntfs-3g/layout.h>
#include <ntfs-3g/attrib.h>
#include <ntfs-3g/bootsect.h>
#include <ntfs-3g/cache.h>
#include <ntfs-3g/device.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/logfile.h>
#include <ntfs-3g/logging.h>
#include <ntfs-3g/ntfstime.h>
#include <ntfs-3g/reparse.h>
#include <ntfs-3g/unistr.h>
#include <ntfs-3g/volume.h>

#include "mkntfs_glue.h"
#include "ntfsb_devio.h"

/* Version of the libntfs-3g sources the bridge is built against. The build
 * passes -DNTFSB_LIBNTFS_VERSION=\"x\"; libntfs-3g has no runtime getter. */
#ifndef NTFSB_LIBNTFS_VERSION
#define NTFSB_LIBNTFS_VERSION "2022.10.3"
#endif

#define NAME_UNITS_MAX 255          /* NTFS file name limit, UTF-16 units */
#define LABEL_UNITS_MAX 32          /* Windows volume label limit */
#define REPARSE_DATA_MAX 16384      /* MAXIMUM_REPARSE_DATA_BUFFER_SIZE */
#define REPARSE_HEADER_SIZE 8       /* tag(4) + data length(2) + reserved(2) */
#define SYMLINK_FLAG_RELATIVE 1u
#define WSL_SYMLINK_VERSION 2u
#define PARENT_WALK_MAX 4096        /* loop guard when walking ".." chains */

struct ntfsb_volume {
    pthread_mutex_t lock;
    ntfs_volume *vol;
    ntfsb_devio *devio;       /* d_private of vol->dev; freed after umount */
    ntfsb_mount_opts opts;
    uint64_t serial;          /* from the boot sector; libntfs-3g drops it */
    bool show_sys;
};

/* ------------------------------------------------------------------------ */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------ */

static int errno_or(int fallback)
{
    return errno > 0 ? errno : fallback;
}

static void vlock(ntfsb_volume *v) { pthread_mutex_lock(&v->lock); }
static void vunlock(ntfsb_volume *v) { pthread_mutex_unlock(&v->lock); }

static int close_inode(ntfs_inode *ni)
{
    if (!ni)
        return 0;
    return ntfs_inode_close(ni) ? errno_or(EIO) : 0;
}

/* Keep the first error: `rc = keep(rc, close_inode(ni))`. */
static int keep(int rc, int next)
{
    return rc ? rc : next;
}

/* NTFS metadata files live in MFT records 0..15; the root (5) is the only
 * one ordinary users see. */
static bool is_metadata_ino(uint64_t ino)
{
    return ino < FILE_first_user && ino != FILE_root;
}

/* ------------------------------------------------------------------------ */
/* Logging                                                                   */
/* ------------------------------------------------------------------------ */

/*
 * libntfs-3g's default handler discards everything. We forward warnings and
 * errors to stderr (the extension's unified log) and drop the chatty levels.
 */
static int bridge_log_handler(const char *function, const char *file, int line,
                              u32 level, void *data, const char *format, va_list args)
{
    const u32 wanted = NTFS_LOG_LEVEL_WARNING | NTFS_LOG_LEVEL_ERROR |
                       NTFS_LOG_LEVEL_PERROR | NTFS_LOG_LEVEL_CRITICAL;
    if (!(level & wanted))
        return 0;
    return ntfs_log_handler_stderr(function, file, line, level, data, format, args);
}

void ntfsb_log_install(void)
{
    ntfs_log_set_handler(bridge_log_handler);
}

static pthread_once_t g_log_once = PTHREAD_ONCE_INIT;

static void ensure_log_handler(void)
{
    pthread_once(&g_log_once, ntfsb_log_install);
}

/* ------------------------------------------------------------------------ */
/* UTF-8 <-> UTF-16LE                                                        */
/*                                                                           */
/* libntfs-3g's ntfs_mbstoucs/ntfs_ucstombs convert through the C locale and */
/* on macOS additionally NFD-normalise names, which would break round-trips  */
/* (lookup of a name returned by readdir). We convert exactly, ourselves.    */
/* ------------------------------------------------------------------------ */

/* Decode one UTF-8 scalar value; returns bytes consumed, 0 if invalid. */
static int utf8_decode(const unsigned char *p, uint32_t *out)
{
    static const uint32_t min_cp[4] = { 0, 0x80, 0x800, 0x10000 };
    uint32_t cp;
    int extra;

    if (p[0] < 0x80) { *out = p[0]; return 1; }
    else if ((p[0] & 0xE0) == 0xC0) { extra = 1; cp = p[0] & 0x1F; }
    else if ((p[0] & 0xF0) == 0xE0) { extra = 2; cp = p[0] & 0x0F; }
    else if ((p[0] & 0xF8) == 0xF0) { extra = 3; cp = p[0] & 0x07; }
    else return 0;
    for (int i = 1; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80)
            return 0;
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    if (cp < min_cp[extra] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return 0; /* overlong, out of range or an encoded surrogate */
    *out = cp;
    return extra + 1;
}

/* UTF-8 → UTF-16LE. EILSEQ on invalid input, ENAMETOOLONG if > cap units. */
static int utf8_to_utf16(const char *s, ntfschar *out, size_t cap, size_t *n_out)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t n = 0;
    while (*p) {
        uint32_t cp;
        int used = utf8_decode(p, &cp);
        if (!used)
            return EILSEQ;
        p += used;
        if (cp >= 0x10000) {
            if (n + 2 > cap)
                return ENAMETOOLONG;
            cp -= 0x10000;
            out[n++] = cpu_to_le16((u16)(0xD800 | (cp >> 10)));
            out[n++] = cpu_to_le16((u16)(0xDC00 | (cp & 0x3FF)));
        } else {
            if (n + 1 > cap)
                return ENAMETOOLONG;
            out[n++] = cpu_to_le16((u16)cp);
        }
    }
    *n_out = n;
    return 0;
}

/* Bytes needed to encode `n` UTF-16 units (worst case 3 per unit) + NUL. */
static size_t utf8_capacity(size_t units)
{
    return units * 3 + 1;
}

/*
 * UTF-16LE → UTF-8, NUL-terminated. NTFS names are arbitrary 16-bit
 * sequences; unpaired surrogates and embedded NULs cannot be expressed in
 * UTF-8 and become U+FFFD. ERANGE if `outsize` is too small.
 */
static int utf16_to_utf8(const ntfschar *in, size_t n, char *out, size_t outsize, size_t *len_out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t cp = le16_to_cpu(in[i]);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n) {
            uint32_t lo = le16_to_cpu(in[i + 1]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        if ((cp >= 0xD800 && cp <= 0xDFFF) || cp == 0)
            cp = 0xFFFD;
        unsigned char buf[4];
        size_t len;
        if (cp < 0x80) { buf[0] = (unsigned char)cp; len = 1; }
        else if (cp < 0x800) {
            buf[0] = (unsigned char)(0xC0 | (cp >> 6));
            buf[1] = (unsigned char)(0x80 | (cp & 0x3F));
            len = 2;
        } else if (cp < 0x10000) {
            buf[0] = (unsigned char)(0xE0 | (cp >> 12));
            buf[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
            buf[2] = (unsigned char)(0x80 | (cp & 0x3F));
            len = 3;
        } else {
            buf[0] = (unsigned char)(0xF0 | (cp >> 18));
            buf[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
            buf[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
            buf[3] = (unsigned char)(0x80 | (cp & 0x3F));
            len = 4;
        }
        if (o + len + 1 > outsize)
            return ERANGE;
        memcpy(out + o, buf, len);
        o += len;
    }
    if (o + 1 > outsize)
        return ERANGE;
    out[o] = '\0';
    if (len_out)
        *len_out = o;
    return 0;
}

static int utf16_to_utf8_alloc(const ntfschar *in, size_t n, char **out)
{
    size_t cap = utf8_capacity(n);
    char *s = malloc(cap);
    if (!s)
        return ENOMEM;
    int err = utf16_to_utf8(in, n, s, cap, NULL);
    if (err) {
        free(s);
        return err;
    }
    *out = s;
    return 0;
}

/* A component name as NTFS stores it. */
typedef struct uname {
    ntfschar s[NAME_UNITS_MAX];
    u8 len;
} uname_t;

static bool is_dot_or_dotdot(const char *name)
{
    return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

/*
 * Validate and convert a single path component. NTFS' POSIX namespace
 * accepts everything except '/' and NUL; "." and ".." are never stored.
 */
static int make_uname(const char *name, uname_t *u)
{
    if (!name || !name[0] || is_dot_or_dotdot(name) || strchr(name, '/'))
        return EINVAL;
    size_t n;
    int err = utf8_to_utf16(name, u->s, NAME_UNITS_MAX, &n);
    if (err)
        return err;
    u->len = (u8)n;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Deterministic volume UUID (RFC 4122 version 5, SHA-1 name-based)          */
/* ------------------------------------------------------------------------ */

typedef struct sha1_ctx {
    uint32_t h[5];
    uint64_t len;
    uint8_t block[64];
    size_t fill;
} sha1_ctx;

static uint32_t rol32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha1_block(sha1_ctx *c, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
               (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & cc) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ cc ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
        else { f = b ^ cc ^ d; k = 0xCA62C1D6; }
        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = rol32(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

static void sha1_update(sha1_ctx *c, const void *data, size_t n)
{
    const uint8_t *p = data;
    c->len += n;
    while (n--) {
        c->block[c->fill++] = *p++;
        if (c->fill == 64) {
            sha1_block(c, c->block);
            c->fill = 0;
        }
    }
}

static void sha1_final(sha1_ctx *c, uint8_t out[20])
{
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    sha1_update(c, &pad, 1);
    pad = 0;
    while (c->fill != 56)
        sha1_update(c, &pad, 1);
    for (int i = 7; i >= 0; i--) {
        uint8_t b = (uint8_t)(bits >> (8 * i));
        sha1_update(c, &b, 1);
    }
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

/*
 * UUID v5 over a project-specific namespace and the 64-bit NTFS serial
 * (little-endian, as stored in the boot sector). Same serial → same UUID on
 * every Mac, which is what FSKit/DiskArbitration want for volume identity.
 */
static void uuid_from_serial(uint64_t serial, uint8_t uuid[16])
{
    static const uint8_t ns[16] = { /* 0f0e7a1c-4b52-5d39-9c2e-6e746673346d */
        0x0f, 0x0e, 0x7a, 0x1c, 0x4b, 0x52, 0x5d, 0x39,
        0x9c, 0x2e, 0x6e, 0x74, 0x66, 0x73, 0x34, 0x6d,
    };
    uint8_t name[8], digest[20];
    for (int i = 0; i < 8; i++)
        name[i] = (uint8_t)(serial >> (8 * i));
    sha1_ctx c = { .h = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 } };
    sha1_update(&c, ns, sizeof(ns));
    sha1_update(&c, name, sizeof(name));
    sha1_final(&c, digest);
    memcpy(uuid, digest, 16);
    uuid[6] = (uint8_t)((uuid[6] & 0x0F) | 0x50); /* version 5 */
    uuid[8] = (uint8_t)((uuid[8] & 0x3F) | 0x80); /* RFC 4122 variant */
}

/* ------------------------------------------------------------------------ */
/* Boot sector                                                               */
/* ------------------------------------------------------------------------ */

typedef struct boot_info {
    bool is_ntfs;
    uint32_t bytes_per_sector;
    uint32_t cluster_size;
    uint64_t total_sectors;
    uint64_t serial;
} boot_info;

static int read_boot_sector(ntfsb_devio *dio, boot_info *bi)
{
    NTFS_BOOT_SECTOR bs;
    memset(bi, 0, sizeof(*bi));
    int64_t r = ntfsb_devio_pread(dio, &bs, sizeof(bs), 0);
    if (r < 0)
        return errno_or(EIO);
    if (r < (int64_t)sizeof(bs) || !ntfs_boot_sector_is_ntfs(&bs))
        return 0;
    bi->is_ntfs = true;
    bi->bytes_per_sector = le16_to_cpu(bs.bpb.bytes_per_sector);
    /* Values above 0x80 encode cluster sizes > 64 KiB as 2^(256 - n) sectors. */
    uint8_t spc = bs.bpb.sectors_per_cluster;
    uint32_t sectors = spc > 0x80 ? 1u << (256 - spc) : spc;
    bi->cluster_size = sectors * bi->bytes_per_sector;
    bi->total_sectors = (uint64_t)sle64_to_cpu(bs.number_of_sectors);
    bi->serial = le64_to_cpu(bs.volume_serial_number);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Volume-level reads                                                        */
/* ------------------------------------------------------------------------ */

/* $Volume/$VOLUME_NAME → UTF-8. A missing attribute is an empty label. */
static int read_label(ntfs_volume *vol, char *out, size_t outsize)
{
    out[0] = '\0';
    ntfs_attr *na = ntfs_attr_open(vol->vol_ni, AT_VOLUME_NAME, AT_UNNAMED, 0);
    if (!na)
        return errno == ENOENT ? 0 : errno_or(EIO);
    int err = 0;
    s64 size = na->data_size;
    if (size > 0) {
        ntfschar buf[128];
        if (size > (s64)sizeof(buf))
            size = sizeof(buf);
        s64 r = ntfs_attr_pread(na, 0, size, buf);
        if (r < 0)
            err = errno_or(EIO);
        else if (utf16_to_utf8(buf, (size_t)r / sizeof(ntfschar), out, outsize, NULL))
            out[0] = '\0'; /* label too long for the header's buffer: report none */
    }
    ntfs_attr_close(na);
    return err;
}

/*
 * Inspect $LogFile like ntfs_device_mount() does for read-write mounts.
 * A restart page of version 2.0 means Windows keeps cached metadata for
 * Fast Startup (or hibernation); an unclean log means the volume was not
 * shut down cleanly.
 */
static void check_logfile(ntfs_volume *vol, bool *unclean, bool *fast_startup)
{
    *unclean = false;
    *fast_startup = false;
    ntfs_inode *ni = ntfs_inode_open(vol, FILE_LogFile);
    if (!ni)
        return;
    ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    if (na) {
        RESTART_PAGE_HEADER *rp = NULL;
        if (!ntfs_check_logfile(na, &rp) || !ntfs_is_logfile_clean(na, rp))
            *unclean = true;
        if (rp && rp->major_ver == const_cpu_to_le16(2) &&
            rp->minor_ver == const_cpu_to_le16(0))
            *fast_startup = true;
        free(rp);
        ntfs_attr_close(na);
    }
    ntfs_inode_close(ni);
}

static bool is_hibernated(ntfs_volume *vol)
{
    errno = 0;
    return ntfs_volume_check_hiberfile(vol, 0) < 0 && errno == EPERM;
}

/* ------------------------------------------------------------------------ */
/* Inodes                                                                    */
/* ------------------------------------------------------------------------ */

static bool mft_record_in_use(ntfs_volume *vol, uint64_t ino)
{
    u8 byte;
    if (ntfs_attr_pread(vol->mftbmp_na, (s64)(ino >> 3), 1, &byte) != 1)
        return true; /* cannot tell: let the original error stand */
    return (byte >> (ino & 7)) & 1;
}

/*
 * Open MFT record `ino` as a base inode. ENOENT for records that are out of
 * range, free, or extension records of another file (stale FSKit items).
 */
static int open_inode(ntfsb_volume *v, uint64_t ino, ntfs_inode **out)
{
    ntfs_volume *vol = v->vol;
    uint64_t nr = (uint64_t)(vol->mft_na->initialized_size >> vol->mft_record_size_bits);
    if (ino >= nr || ino > MFT_REF_MASK_CPU)
        return ENOENT;
    ntfs_inode *ni = ntfs_inode_open(vol, (MFT_REF)ino);
    if (!ni) {
        int err = errno_or(EIO);
        if (err == EIO && !mft_record_in_use(vol, ino))
            err = ENOENT;
        return err;
    }
    if (ni->mrec->base_mft_record) {
        ntfs_inode_close(ni);
        return ENOENT;
    }
    *out = ni;
    return 0;
}

static bool inode_is_dir(const ntfs_inode *ni)
{
    return (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) != 0;
}

static int open_dir(ntfsb_volume *v, uint64_t ino, ntfs_inode **out)
{
    ntfs_inode *ni;
    int err = open_inode(v, ino, &ni);
    if (err)
        return err;
    if (!inode_is_dir(ni)) {
        ntfs_inode_close(ni);
        return ENOTDIR;
    }
    *out = ni;
    return 0;
}

/*
 * Walk the FILE_NAME attributes: count hard links and pick the parent.
 * DOS (8.3) names are aliases of a Win32 name in the same directory, not
 * separate links, so they are not counted (the on-disk link_count does
 * count them).
 */
static void scan_names(ntfs_inode *ni, uint32_t *nlink, uint64_t *parent)
{
    uint32_t links = 0;
    uint64_t par = 0, dos_par = 0;
    ntfs_attr_search_ctx *ctx = ntfs_attr_get_search_ctx(ni, NULL);
    if (ctx) {
        while (!ntfs_attr_lookup(AT_FILE_NAME, AT_UNNAMED, 0, CASE_SENSITIVE, 0, NULL, 0, ctx)) {
            if (ctx->attr->non_resident)
                continue;
            const FILE_NAME_ATTR *fn = (const FILE_NAME_ATTR *)
                ((const u8 *)ctx->attr + le16_to_cpu(ctx->attr->value_offset));
            if (fn->file_name_type == FILE_NAME_DOS) {
                if (!dos_par)
                    dos_par = MREF_LE(fn->parent_directory);
                continue;
            }
            links++;
            if (!par)
                par = MREF_LE(fn->parent_directory);
        }
        ntfs_attr_put_search_ctx(ctx);
    }
    if (nlink)
        *nlink = links ? links : le16_to_cpu(ni->mrec->link_count);
    if (parent)
        *parent = par ? par : dos_par;
}

static int parent_of(ntfsb_volume *v, uint64_t ino, uint64_t *parent)
{
    ntfs_inode *ni;
    int err = open_inode(v, ino, &ni);
    if (err)
        return err;
    scan_names(ni, NULL, parent);
    err = close_inode(ni);
    if (!err && !*parent)
        err = ENOENT;
    return err;
}

/* ------------------------------------------------------------------------ */
/* Symbolic links                                                            */
/*                                                                           */
/* Windows knows three kinds of link-like reparse points and we read all of  */
/* them; older ntfs-3g/SFU volumes also contain Interix symlinks (system     */
/* files whose $DATA is "IntxLNK\1" + UTF-16 target).                        */
/* ------------------------------------------------------------------------ */

static const u8 intx_symlink_magic[8] = { 'I', 'n', 't', 'x', 'L', 'N', 'K', 1 };

static uint16_t get_le16(const u8 *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get_le32(const u8 *p) { return (uint32_t)get_le16(p) | (uint32_t)get_le16(p + 2) << 16; }
static void put_le16(u8 *p, uint16_t x) { p[0] = (u8)x; p[1] = (u8)(x >> 8); }
static void put_le32(u8 *p, uint32_t x) { put_le16(p, (uint16_t)x); put_le16(p + 2, (uint16_t)(x >> 16)); }

#define TAG_MOUNT_POINT 0xA0000003u
#define TAG_SYMLINK     0xA000000Cu
#define TAG_LX_SYMLINK  0xA000001Du

static bool tag_is_link(uint32_t tag)
{
    return tag == TAG_MOUNT_POINT || tag == TAG_SYMLINK || tag == TAG_LX_SYMLINK;
}

/* Replace Windows separators with POSIX ones in place. */
static void backslash_to_slash(char *s)
{
    for (; *s; s++)
        if (*s == '\\')
            *s = '/';
}

/* Number of ".." hops from directory `dir` up to the volume root. */
static int depth_to_root(ntfsb_volume *v, uint64_t dir, unsigned *depth)
{
    unsigned d = 0;
    while (dir != FILE_root) {
        if (++d > PARENT_WALK_MAX)
            return ELOOP;
        int err = parent_of(v, dir, &dir);
        if (err)
            return err;
    }
    *depth = d;
    return 0;
}

/*
 * An absolute Windows target ("\??\C:\dir\file" or "C:\dir\file") has no
 * POSIX meaning: we do not know where the volume is mounted, and drive
 * letters do not exist. Assume the target lives on this volume (the usual
 * case for junctions on removable disks) and turn it into a path relative
 * to the link's directory, which works wherever the volume is mounted.
 * Other forms (volume GUID paths, UNC) are returned verbatim with '/'.
 */
static int absolute_target(ntfsb_volume *v, uint64_t link_parent, char *path, char **out)
{
    char *p = path;
    if (strncmp(p, "\\??\\", 4) == 0)
        p += 4;
    bool drive = ((p[0] | 0x20) >= 'a' && (p[0] | 0x20) <= 'z') && p[1] == ':' &&
                 (p[2] == '\\' || p[2] == '\0');
    if (!drive) {
        char *s = strdup(path);
        if (!s)
            return ENOMEM;
        backslash_to_slash(s);
        *out = s;
        return 0;
    }
    const char *rest = p[2] ? p + 3 : p + 2;
    unsigned depth;
    int err = depth_to_root(v, link_parent, &depth);
    if (err)
        return err;
    size_t len = depth * 3 + strlen(rest) + 2;
    char *s = malloc(len);
    if (!s)
        return ENOMEM;
    s[0] = '\0';
    for (unsigned i = 0; i < depth; i++)
        strcat(s, "../");
    strcat(s, rest);
    size_t sl = strlen(s);
    if (sl == 0)
        strcpy(s, ".");
    else if (s[sl - 1] == '/')
        s[sl - 1] = '\0'; /* "../" alone → ".." */
    backslash_to_slash(s);
    *out = s;
    return 0;
}

/* Parse a name from a SYMLINK / MOUNT_POINT reparse path buffer. */
static int reparse_name(const u8 *pathbuf, size_t pathbuf_len, uint16_t off, uint16_t len,
                        char **out)
{
    if ((off & 1) || (len & 1) || (size_t)off + len > pathbuf_len || len == 0)
        return EIO;
    ntfschar units[REPARSE_DATA_MAX / 2];
    memcpy(units, pathbuf + off, len);
    return utf16_to_utf8_alloc(units, len / 2, out);
}

/*
 * Resolve the link target of `ni`. *is_link reports whether the inode is a
 * link at all (even if its data turns out to be malformed). Returns 0 with
 * a malloc'ed UTF-8 target, EINVAL if not a link, or another errno.
 */
static int link_target(ntfsb_volume *v, ntfs_inode *ni, char **out, bool *is_link)
{
    *is_link = false;
    if (ni->flags & FILE_ATTR_REPARSE_POINT) {
        s64 size = 0;
        u8 *rp = ntfs_attr_readall(ni, AT_REPARSE_POINT, AT_UNNAMED, 0, &size);
        if (!rp)
            return errno == ENOENT ? EINVAL : errno_or(EIO);
        int err = EINVAL;
        uint32_t tag = size >= REPARSE_HEADER_SIZE ? get_le32(rp) : 0;
        if (tag_is_link(tag)) {
            *is_link = true;
            size_t dlen = get_le16(rp + 4);
            const u8 *d = rp + REPARSE_HEADER_SIZE;
            err = EIO;
            if (REPARSE_HEADER_SIZE + dlen > (size_t)size) {
                /* malformed */
            } else if (tag == TAG_LX_SYMLINK) {
                /* WSL: le32 version 2, then the UTF-8 target without NUL. */
                if (dlen > 4 && get_le32(d) == WSL_SYMLINK_VERSION) {
                    char *s = malloc(dlen - 4 + 1);
                    if (!s) {
                        err = ENOMEM;
                    } else {
                        memcpy(s, d + 4, dlen - 4);
                        s[dlen - 4] = '\0';
                        *out = s;
                        err = 0;
                    }
                }
            } else {
                /* SYMLINK: 4 x le16 name offsets/lengths + le32 flags + path
                 * buffer; MOUNT_POINT: the same without the flags field. */
                size_t hdr = tag == TAG_SYMLINK ? 12 : 8;
                if (dlen >= hdr) {
                    bool relative = tag == TAG_SYMLINK && (get_le32(d + 8) & SYMLINK_FLAG_RELATIVE);
                    char *name = NULL;
                    err = reparse_name(d + hdr, dlen - hdr, get_le16(d), get_le16(d + 2), &name);
                    if (!err && relative) {
                        backslash_to_slash(name);
                        *out = name;
                    } else if (!err) {
                        uint64_t parent;
                        scan_names(ni, NULL, &parent);
                        err = absolute_target(v, parent ? parent : FILE_root, name, out);
                        free(name);
                    }
                }
            }
        }
        free(rp);
        return err;
    }

    if ((ni->flags & FILE_ATTR_SYSTEM) && !inode_is_dir(ni)) {
        ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
        if (!na)
            return errno == ENOENT ? EINVAL : errno_or(EIO);
        int err = EINVAL;
        s64 size = na->data_size;
        if (size > (s64)sizeof(intx_symlink_magic) && size <= REPARSE_DATA_MAX &&
            ((size - (s64)sizeof(intx_symlink_magic)) & 1) == 0) {
            u8 *buf = malloc((size_t)size);
            if (!buf) {
                err = ENOMEM;
            } else if (ntfs_attr_pread(na, 0, size, buf) != size) {
                err = errno_or(EIO);
            } else if (!memcmp(buf, intx_symlink_magic, sizeof(intx_symlink_magic))) {
                *is_link = true;
                size_t units = (size_t)(size - (s64)sizeof(intx_symlink_magic)) / 2;
                ntfschar *target = malloc(units * sizeof(ntfschar));
                if (!target) {
                    err = ENOMEM;
                } else {
                    memcpy(target, buf + sizeof(intx_symlink_magic), units * sizeof(ntfschar));
                    err = utf16_to_utf8_alloc(target, units, out);
                    free(target);
                }
            }
            free(buf);
        }
        ntfs_attr_close(na);
        return err;
    }
    return EINVAL;
}

/* ------------------------------------------------------------------------ */
/* Attributes                                                                */
/* ------------------------------------------------------------------------ */

static uint32_t flags_from_ntfs(const ntfs_inode *ni)
{
    uint32_t f = 0;
    /* Windows ignores READONLY on directories (Explorer uses it to mark
     * customised folders), so it is not reported for them. */
    if ((ni->flags & FILE_ATTR_READONLY) && !inode_is_dir(ni)) f |= NTFSB_FLAG_READONLY;
    if (ni->flags & FILE_ATTR_HIDDEN)      f |= NTFSB_FLAG_HIDDEN;
    if (ni->flags & FILE_ATTR_SYSTEM)      f |= NTFSB_FLAG_SYSTEM;
    if (ni->flags & FILE_ATTR_ARCHIVE)     f |= NTFSB_FLAG_ARCHIVE;
    if (ni->flags & FILE_ATTR_COMPRESSED)  f |= NTFSB_FLAG_COMPRESSED;
    if (ni->flags & FILE_ATTR_ENCRYPTED)   f |= NTFSB_FLAG_ENCRYPTED;
    if (ni->flags & FILE_ATTR_SPARSE_FILE) f |= NTFSB_FLAG_SPARSE;
    return f;
}

static int fill_stat(ntfsb_volume *v, ntfs_inode *ni, ntfsb_stat *st)
{
    memset(st, 0, sizeof(*st));
    st->ino = ni->mft_no;
    st->generation = le16_to_cpu(ni->mrec->sequence_number);
    st->uid = v->opts.uid;
    st->gid = v->opts.gid;
    st->flags = flags_from_ntfs(ni);
    st->atime = ntfs2timespec(ni->last_access_time);
    st->mtime = ntfs2timespec(ni->last_data_change_time);
    st->ctime = ntfs2timespec(ni->last_mft_change_time);
    st->btime = ntfs2timespec(ni->creation_time);
    scan_names(ni, &st->nlink, &st->parent_ino);
    if (ni->mft_no == FILE_root)
        st->parent_ino = FILE_root;

    bool is_link = false;
    char *target = NULL;
    if (ni->flags & (FILE_ATTR_REPARSE_POINT | FILE_ATTR_SYSTEM)) {
        int err = link_target(v, ni, &target, &is_link);
        if (err && !is_link && err != EINVAL)
            return err;
    }

    if (is_link) {
        /* A malformed link still is a link; readlink reports the error. */
        st->type = NTFSB_TYPE_SYMLINK;
        st->mode = S_IFLNK | 0777;
        st->size = target ? strlen(target) : 0;
        st->alloc_size = (uint64_t)ni->allocated_size;
        free(target);
    } else if (inode_is_dir(ni)) {
        st->type = NTFSB_TYPE_DIR;
        st->mode = S_IFDIR | (0777 & ~v->opts.dmask);
        /* Directory "size" is the size of its B+tree index, as in ntfs-3g. */
        ntfs_attr *na = ntfs_attr_open(ni, AT_INDEX_ALLOCATION, NTFS_INDEX_I30, 4);
        if (na) {
            st->size = (uint64_t)na->data_size;
            st->alloc_size = (uint64_t)na->allocated_size;
            ntfs_attr_close(na);
        } else {
            st->size = (uint64_t)ni->data_size;
            st->alloc_size = (uint64_t)ni->allocated_size;
        }
        /* NTFS does not track subdirectory counts; 1 tells fts(3) and
         * friends not to rely on the "2 + subdirs" convention. */
        st->nlink = 1;
    } else {
        st->type = NTFSB_TYPE_FILE;
        st->mode = S_IFREG | (0666 & ~v->opts.fmask);
        if (ni->flags & FILE_ATTR_READONLY)
            st->mode &= ~0222u;
        st->size = (uint64_t)ni->data_size;
        st->alloc_size = (uint64_t)ni->allocated_size;
    }
    return 0;
}

int ntfsb_getattr(ntfsb_volume *vol, uint64_t ino, ntfsb_stat *out)
{
    if (!vol || !out)
        return EINVAL;
    vlock(vol);
    ntfs_inode *ni;
    int err = open_inode(vol, ino, &ni);
    if (!err) {
        err = fill_stat(vol, ni, out);
        err = keep(err, close_inode(ni));
    }
    vunlock(vol);
    return err;
}

static bool is_symlink_inode(ntfsb_volume *v, ntfs_inode *ni)
{
    if (!(ni->flags & (FILE_ATTR_REPARSE_POINT | FILE_ATTR_SYSTEM)))
        return false;
    bool is_link;
    char *target = NULL;
    link_target(v, ni, &target, &is_link);
    free(target);
    return is_link;
}

/* Truncate the unnamed $DATA stream of an open inode (shared by setattr and truncate). */
static int truncate_open_inode(ntfsb_volume *v, ntfs_inode *ni, uint64_t size)
{
    if (inode_is_dir(ni))
        return EISDIR;
    if (is_symlink_inode(v, ni))
        return EINVAL;
    if (ni->flags & FILE_ATTR_ENCRYPTED)
        return EACCES;
    if (size > (uint64_t)INT64_MAX)
        return EFBIG;
    ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    if (!na)
        return errno_or(EIO);
    int err = 0;
    s64 old = na->data_size;
    if ((s64)size != old) {
        /*
         * Growing a compressed stream with ntfs_attr_truncate() would
         * allocate and zero real clusters; writing one zero byte at the
         * new end lets the compression code create a hole instead
         * (same approach as the ntfs-3g FUSE driver).
         */
        if ((na->data_flags & ATTR_COMPRESSION_MASK) && (s64)size > na->initialized_size) {
            char zero = 0;
            if (ntfs_attr_pwrite(na, (s64)size - 1, 1, &zero) != 1)
                err = errno_or(EIO);
            else if (ntfs_attr_pclose(na))
                err = errno_or(EIO);
        } else if (ntfs_attr_truncate(na, (s64)size)) {
            err = errno_or(EIO);
        }
        if (!err)
            ni->flags |= FILE_ATTR_ARCHIVE;
    }
    ntfs_attr_close(na);
    if (!err)
        ntfs_inode_update_times(ni, NTFS_UPDATE_MCTIME);
    return err;
}

int ntfsb_setattr(ntfsb_volume *vol, uint64_t ino, const ntfsb_setattr_req *req, ntfsb_stat *out)
{
    if (!vol || !req)
        return EINVAL;
    /* Ownership cannot be represented without a user mapping: accepted, ignored. */
    const uint32_t effective = req->mask & ~(uint32_t)(NTFSB_SET_UID | NTFSB_SET_GID);
    vlock(vol);
    ntfs_inode *ni;
    int err = open_inode(vol, ino, &ni);
    if (err)
        goto out;

    if (effective) {
        if (NVolReadOnly(vol->vol)) {
            err = EROFS;
        } else if (is_metadata_ino(ino)) {
            err = EPERM;
        }
    }
    if (!err && (effective & NTFSB_SET_SIZE))
        err = truncate_open_inode(vol, ni, req->size);

    if (!err && (effective & (NTFSB_SET_MODE | NTFSB_SET_FLAGS))) {
        FILE_ATTR_FLAGS f = ni->flags;
        if (effective & NTFSB_SET_FLAGS) {
            f &= ~(FILE_ATTR_READONLY | FILE_ATTR_HIDDEN | FILE_ATTR_SYSTEM | FILE_ATTR_ARCHIVE);
            if (req->flags & NTFSB_FLAG_READONLY) f |= FILE_ATTR_READONLY;
            if (req->flags & NTFSB_FLAG_HIDDEN)   f |= FILE_ATTR_HIDDEN;
            if (req->flags & NTFSB_FLAG_SYSTEM)   f |= FILE_ATTR_SYSTEM;
            if (req->flags & NTFSB_FLAG_ARCHIVE)  f |= FILE_ATTR_ARCHIVE;
        }
        /* The only POSIX permission NTFS can store is "not writable". A
         * mode change wins over a simultaneous flags change for READONLY. */
        if (effective & NTFSB_SET_MODE) {
            if (req->mode & 0222)
                f &= ~FILE_ATTR_READONLY;
            else
                f |= FILE_ATTR_READONLY;
        }
        if (inode_is_dir(ni))
            f = (f & ~FILE_ATTR_READONLY) | (ni->flags & FILE_ATTR_READONLY);
        if (f != ni->flags) {
            ni->flags = f;
            NInoFileNameSetDirty(ni); /* flags are mirrored in the parent index */
            ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
        }
    }

    if (!err && (effective & (NTFSB_SET_ATIME | NTFSB_SET_MTIME | NTFSB_SET_CTIME | NTFSB_SET_BTIME))) {
        if (!(effective & NTFSB_SET_CTIME))
            ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
        if (effective & NTFSB_SET_ATIME) ni->last_access_time = timespec2ntfs(req->atime);
        if (effective & NTFSB_SET_MTIME) ni->last_data_change_time = timespec2ntfs(req->mtime);
        if (effective & NTFSB_SET_CTIME) ni->last_mft_change_time = timespec2ntfs(req->ctime);
        if (effective & NTFSB_SET_BTIME) ni->creation_time = timespec2ntfs(req->btime);
        /* Times live in $STANDARD_INFORMATION and are mirrored into the
         * FILE_NAME index entries; both are rewritten on close. */
        NInoFileNameSetDirty(ni);
        NInoSetDirty(ni);
    }

    if (!err && out)
        err = fill_stat(vol, ni, out);
    err = keep(err, close_inode(ni));
out:
    vunlock(vol);
    return err;
}

/* ------------------------------------------------------------------------ */
/* Namespace                                                                 */
/* ------------------------------------------------------------------------ */

/* Look `u` up in an open directory. ENOENT if absent. */
static int lookup_in(ntfs_inode *dir_ni, const uname_t *u, uint64_t *ino)
{
    errno = 0;
    u64 mref = ntfs_inode_lookup_by_name(dir_ni, u->s, u->len);
    if (mref == (u64)-1)
        return errno_or(ENOENT);
    *ino = MREF(mref);
    return 0;
}

static int lookup_ino(ntfsb_volume *v, uint64_t dir_ino, const uname_t *u, uint64_t *ino)
{
    ntfs_inode *dir_ni;
    int err = open_dir(v, dir_ino, &dir_ni);
    if (err)
        return err;
    err = lookup_in(dir_ni, u, ino);
    return keep(err, close_inode(dir_ni));
}

int ntfsb_lookup(ntfsb_volume *vol, uint64_t dir_ino, const char *name_utf8, ntfsb_stat *out)
{
    if (!vol || !name_utf8 || !out)
        return EINVAL;
    vlock(vol);
    uint64_t ino = dir_ino;
    int err = 0;

    if (strcmp(name_utf8, ".") == 0 || strcmp(name_utf8, "..") == 0) {
        ntfs_inode *dir_ni;
        err = open_dir(vol, dir_ino, &dir_ni);
        if (!err) {
            if (name_utf8[1] == '.' && dir_ino != FILE_root)
                scan_names(dir_ni, NULL, &ino);
            err = close_inode(dir_ni);
        }
    } else {
        uname_t u;
        err = make_uname(name_utf8, &u);
        if (!err)
            err = lookup_ino(vol, dir_ino, &u, &ino);
        if (!err && !vol->show_sys && is_metadata_ino(ino))
            err = ENOENT;
    }
    if (!err) {
        ntfs_inode *ni;
        err = open_inode(vol, ino, &ni);
        if (!err) {
            err = fill_stat(vol, ni, out);
            err = keep(err, close_inode(ni));
        }
    }
    vunlock(vol);
    return err;
}

/*
 * readdir cookies.
 *
 * ntfs_readdir() keeps a position `pos` with this layout:
 *   0                      "."
 *   1                      ".."
 *   2 .. mft_record_size-1 byte offset of an entry inside $INDEX_ROOT
 *                          (entries start well past offset 2)
 *   mft_record_size + x    byte offset x inside $INDEX_ALLOCATION
 *                          (index block vcn << vcn bits + offset in block)
 * Before handing an entry to the filldir callback it sets pos to that
 * entry's own position, and when (re)started at position P it skips every
 * entry whose position is < P. Entry positions are strictly increasing and
 * every entry is at least 16 bytes long, so "pos + 1" lies strictly
 * between an entry and its successor: resuming there skips exactly the
 * entries already returned, in both the root and the allocation, and
 * across the root → allocation boundary. Hence next_cookie = pos + 1, which
 * also yields the documented cookies 1 and 2 for "." and "..".
 *
 * Cookies are byte offsets in the B+tree, so concurrent inserts/deletes can
 * shift entries across a cookie (an entry may be skipped or repeated), as
 * with every offset-cookie file system.
 */
typedef struct readdir_ctx {
    ntfsb_volume *v;
    ntfsb_dirent_fn fn;
    void *fn_ctx;
    bool stopped;
    int err;
    char name[NTFSB_MAX_NAME_UTF8];
} readdir_ctx;

static ntfsb_type type_from_dt(ntfsb_volume *v, unsigned dt_type, uint64_t ino)
{
    switch (dt_type) {
    case NTFS_DT_DIR: return NTFSB_TYPE_DIR;
    case NTFS_DT_LNK: return NTFSB_TYPE_SYMLINK;
    case NTFS_DT_REPARSE: {
        /* A reparse point that is not a link (dedup, cloud placeholder...):
         * its kind is whatever the MFT record says. */
        ntfs_inode *ni;
        if (open_inode(v, ino, &ni))
            return NTFSB_TYPE_UNKNOWN;
        ntfsb_type t = inode_is_dir(ni) ? NTFSB_TYPE_DIR : NTFSB_TYPE_FILE;
        ntfs_inode_close(ni);
        return t;
    }
    case NTFS_DT_UNKNOWN: return NTFSB_TYPE_UNKNOWN;
    default:
        /* Regular files and Interix FIFOs/sockets/devices, which getattr
         * also reports as regular files. */
        return NTFSB_TYPE_FILE;
    }
}

static int readdir_filldir(void *dirent, const ntfschar *name, const int name_len,
                           const int name_type, const s64 pos, const MFT_REF mref,
                           const unsigned dt_type)
{
    readdir_ctx *rc = dirent;
    uint64_t ino = MREF(mref);

    /* 8.3 aliases share the inode of the long name listed next to them. */
    if (name_type == FILE_NAME_DOS)
        return 0;
    /* pos 0/1 are the synthesised "." and ".."; everything else is real. */
    if (pos >= 2 && !rc->v->show_sys && is_metadata_ino(ino))
        return 0;

    int err = utf16_to_utf8(name, (size_t)name_len, rc->name, sizeof(rc->name), NULL);
    if (err) {
        rc->err = err;
        return -1;
    }
    ntfsb_type type = pos < 2 ? NTFSB_TYPE_DIR : type_from_dt(rc->v, dt_type, ino);
    if (rc->fn(rc->fn_ctx, rc->name, ino, type, (uint64_t)pos + 1)) {
        rc->stopped = true;
        return -1;
    }
    return 0;
}

int ntfsb_readdir(ntfsb_volume *vol, uint64_t dir_ino, uint64_t cookie,
                  ntfsb_dirent_fn fn, void *ctx, bool *out_eof)
{
    if (!vol || !fn)
        return EINVAL;
    vlock(vol);
    ntfs_inode *dir_ni;
    int err = open_dir(vol, dir_ino, &dir_ni);
    if (err)
        goto out;

    /* A cookie past the end of the index cannot have come from us. */
    s64 end = vol->vol->mft_record_size;
    ntfs_attr *ia = ntfs_attr_open(dir_ni, AT_INDEX_ALLOCATION, NTFS_INDEX_I30, 4);
    if (ia) {
        end += ia->data_size;
        ntfs_attr_close(ia);
    }
    if (cookie > (uint64_t)end) {
        err = EINVAL;
        ntfs_inode_close(dir_ni);
        goto out;
    }

    readdir_ctx rc = { .v = vol, .fn = fn, .fn_ctx = ctx };
    s64 pos = (s64)cookie;
    /*
     * With ignore-case enabled libntfs-3g lowercases every name it lists.
     * We want case-preserving listings with case-insensitive lookups, so
     * list in case-sensitive mode (safe: we hold the volume lock).
     */
    bool ci = !NVolCaseSensitive(vol->vol);
    if (ci)
        NVolSetCaseSensitive(vol->vol);
    int r = ntfs_readdir(dir_ni, &pos, &rc, readdir_filldir);
    int rerr = errno_or(EIO);
    if (ci)
        NVolClearCaseSensitive(vol->vol);

    if (rc.err)
        err = rc.err;
    else if (r && !rc.stopped)
        err = rerr;
    err = keep(err, close_inode(dir_ni));
    if (!err && out_eof)
        *out_eof = !rc.stopped;
out:
    vunlock(vol);
    return err;
}

static int check_writable_dir(ntfsb_volume *v, uint64_t dir_ino)
{
    if (NVolReadOnly(v->vol))
        return EROFS;
    /* $Extend and the other metadata directories are off limits. */
    if (is_metadata_ino(dir_ino))
        return EPERM;
    return 0;
}

/*
 * Resolve a relative symlink target starting at `dir` without following
 * links. Used only to decide how to store a new symlink.
 */
static int resolve_relative(ntfsb_volume *v, uint64_t dir, const char *target, bool *is_dir)
{
    char *copy = strdup(target);
    if (!copy)
        return ENOMEM;
    uint64_t cur = dir;
    int err = 0;
    char *save = NULL;
    for (char *c = strtok_r(copy, "/", &save); c && !err; c = strtok_r(NULL, "/", &save)) {
        if (strcmp(c, ".") == 0)
            continue;
        if (strcmp(c, "..") == 0) {
            err = cur == FILE_root ? ENOENT : parent_of(v, cur, &cur);
            continue;
        }
        uname_t u;
        ntfs_inode *ni;
        err = make_uname(c, &u);
        if (!err)
            err = open_dir(v, cur, &ni);
        if (err)
            break;
        if (ni->flags & FILE_ATTR_REPARSE_POINT)
            err = ENOENT; /* would traverse a link */
        else
            err = lookup_in(ni, &u, &cur);
        err = keep(err, close_inode(ni));
    }
    free(copy);
    if (err)
        return err;
    ntfs_inode *ni;
    err = open_inode(v, cur, &ni);
    if (err)
        return err;
    if (ni->flags & FILE_ATTR_REPARSE_POINT)
        err = ENOENT;
    *is_dir = inode_is_dir(ni);
    return keep(err, close_inode(ni));
}

/*
 * Build the reparse buffer for a new symlink.
 *
 * Relative targets that resolve on this volume become native Windows
 * symlinks (IO_REPARSE_TAG_SYMLINK, SYMLINK_FLAG_RELATIVE) that Windows
 * itself follows; a target that is a directory needs a directory symlink.
 * Everything else — absolute POSIX paths, dangling targets, names with
 * '\' or ':' that Windows would misparse — is stored as a WSL symlink
 * (IO_REPARSE_TAG_LX_SYMLINK), which keeps the exact UTF-8 bytes and is
 * understood by WSL and ntfs-3g.
 */
static int build_symlink_reparse(ntfsb_volume *v, uint64_t dir_ino, const char *target,
                                 u8 **out, size_t *out_len, bool *as_dir)
{
    size_t tlen = strlen(target);
    bool native = target[0] != '/' && !strchr(target, '\\') && !strchr(target, ':');
    bool target_is_dir = false;
    if (native && resolve_relative(v, dir_ino, target, &target_is_dir))
        native = false;
    *as_dir = native && target_is_dir;

    if (native) {
        ntfschar units[REPARSE_DATA_MAX / 2];
        size_t n;
        int err = utf8_to_utf16(target, units, sizeof(units) / sizeof(units[0]), &n);
        if (err)
            return err;
        for (size_t i = 0; i < n; i++)
            if (units[i] == const_cpu_to_le16('/'))
                units[i] = const_cpu_to_le16('\\');
        size_t name_bytes = n * sizeof(ntfschar);
        size_t data_len = 12 + 2 * name_bytes;
        if (data_len > REPARSE_DATA_MAX - REPARSE_HEADER_SIZE)
            return ENAMETOOLONG;
        u8 *b = calloc(1, REPARSE_HEADER_SIZE + data_len);
        if (!b)
            return ENOMEM;
        put_le32(b, TAG_SYMLINK);
        put_le16(b + 4, (uint16_t)data_len);
        u8 *d = b + REPARSE_HEADER_SIZE;
        put_le16(d + 0, 0);                        /* substitute name offset */
        put_le16(d + 2, (uint16_t)name_bytes);     /* substitute name length */
        put_le16(d + 4, (uint16_t)name_bytes);     /* print name offset */
        put_le16(d + 6, (uint16_t)name_bytes);     /* print name length */
        put_le32(d + 8, SYMLINK_FLAG_RELATIVE);
        memcpy(d + 12, units, name_bytes);
        memcpy(d + 12 + name_bytes, units, name_bytes);
        *out = b;
        *out_len = REPARSE_HEADER_SIZE + data_len;
        return 0;
    }

    size_t data_len = 4 + tlen;
    if (data_len > REPARSE_DATA_MAX - REPARSE_HEADER_SIZE)
        return ENAMETOOLONG;
    u8 *b = calloc(1, REPARSE_HEADER_SIZE + data_len);
    if (!b)
        return ENOMEM;
    put_le32(b, TAG_LX_SYMLINK);
    put_le16(b + 4, (uint16_t)data_len);
    put_le32(b + REPARSE_HEADER_SIZE, WSL_SYMLINK_VERSION);
    memcpy(b + REPARSE_HEADER_SIZE + 4, target, tlen);
    *out = b;
    *out_len = REPARSE_HEADER_SIZE + data_len;
    return 0;
}

/*
 * Common creation path. `reparse` (optional) is attached to the new inode;
 * on failure the half-made inode is deleted again.
 */
static int create_common(ntfsb_volume *v, uint64_t dir_ino, const uname_t *u, bool dir,
                         bool readonly, const u8 *reparse, size_t reparse_len, ntfsb_stat *out)
{
    ntfs_inode *dir_ni;
    int err = open_dir(v, dir_ino, &dir_ni);
    if (err)
        return err;
    uint64_t existing;
    err = lookup_in(dir_ni, u, &existing);
    if (!err) {
        ntfs_inode_close(dir_ni);
        return EEXIST;
    }
    if (err != ENOENT) {
        ntfs_inode_close(dir_ni);
        return err;
    }

    /* securid 0: no inheritable security id; ntfs_create() then attaches
     * an "Everyone: full control" descriptor, as ntfs-3g does without a
     * user mapping. */
    ntfs_inode *ni = ntfs_create(dir_ni, const_cpu_to_le32(0), u->s, u->len,
                                 dir ? S_IFDIR : S_IFREG);
    if (!ni) {
        err = errno_or(EIO);
        ntfs_inode_close(dir_ni);
        return err;
    }
    if (reparse && ntfs_set_ntfs_reparse_data(ni, (const char *)reparse, reparse_len, 0)) {
        err = errno_or(EIO);
        /* ntfs_delete() closes both inodes, even on failure. */
        ntfs_delete(v->vol, NULL, ni, dir_ni, u->s, u->len);
        return err;
    }
    if (readonly)
        ni->flags |= FILE_ATTR_READONLY;
    NInoFileNameSetDirty(ni);
    NInoSetDirty(ni);
    if (out)
        err = fill_stat(v, ni, out);
    /* Closing the child syncs its FILE_NAME into dir_ni's index; doing it
     * through dir_ni avoids opening the directory a second time. */
    if (ntfs_inode_close_in_dir(ni, dir_ni))
        err = keep(err, errno_or(EIO));
    ntfs_inode_update_times(dir_ni, NTFS_UPDATE_MCTIME);
    return keep(err, close_inode(dir_ni));
}

int ntfsb_create(ntfsb_volume *vol, uint64_t dir_ino, const char *name_utf8,
                 ntfsb_type type, uint32_t mode, ntfsb_stat *out)
{
    if (!vol || (type != NTFSB_TYPE_FILE && type != NTFSB_TYPE_DIR))
        return EINVAL;
    uname_t u;
    int err = make_uname(name_utf8, &u);
    if (err)
        return err;
    vlock(vol);
    err = check_writable_dir(vol, dir_ino);
    if (!err) {
        bool dir = type == NTFSB_TYPE_DIR;
        err = create_common(vol, dir_ino, &u, dir, !dir && !(mode & 0222), NULL, 0, out);
    }
    vunlock(vol);
    return err;
}

int ntfsb_symlink(ntfsb_volume *vol, uint64_t dir_ino, const char *name_utf8,
                  const char *target_utf8, ntfsb_stat *out)
{
    if (!vol || !target_utf8)
        return EINVAL;
    if (!target_utf8[0])
        return ENOENT; /* POSIX: empty symlink target */
    uname_t u;
    int err = make_uname(name_utf8, &u);
    if (err)
        return err;
    vlock(vol);
    err = check_writable_dir(vol, dir_ino);
    if (!err) {
        u8 *rp = NULL;
        size_t rp_len = 0;
        bool as_dir = false;
        err = build_symlink_reparse(vol, dir_ino, target_utf8, &rp, &rp_len, &as_dir);
        if (!err)
            err = create_common(vol, dir_ino, &u, as_dir, false, rp, rp_len, out);
        free(rp);
    }
    vunlock(vol);
    return err;
}

int ntfsb_readlink(ntfsb_volume *vol, uint64_t ino, char *buf, size_t bufsize, size_t *out_len)
{
    if (!vol || !buf)
        return EINVAL;
    vlock(vol);
    ntfs_inode *ni;
    int err = open_inode(vol, ino, &ni);
    if (!err) {
        char *target = NULL;
        bool is_link;
        err = link_target(vol, ni, &target, &is_link);
        if (!err) {
            size_t len = strlen(target);
            if (len + 1 > bufsize) {
                err = ERANGE;
            } else {
                memcpy(buf, target, len + 1);
                if (out_len)
                    *out_len = len;
            }
            free(target);
        }
        err = keep(err, close_inode(ni));
    }
    vunlock(vol);
    return err;
}

/* Add name `u` in dir_ino for inode ino (opens and closes both). */
static int link_ino(ntfsb_volume *v, uint64_t ino, uint64_t dir_ino, const uname_t *u)
{
    ntfs_inode *ni, *dir_ni;
    int err = open_inode(v, ino, &ni);
    if (err)
        return err;
    err = open_dir(v, dir_ino, &dir_ni);
    if (err) {
        ntfs_inode_close(ni);
        return err;
    }
    if (ntfs_link(ni, dir_ni, u->s, u->len)) {
        err = errno_or(EIO);
    } else {
        ni->flags |= FILE_ATTR_ARCHIVE;
        ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
        ntfs_inode_update_times(dir_ni, NTFS_UPDATE_MCTIME);
    }
    /* The directory first: closing ni re-syncs its FILE_NAME index entries,
     * which opens the parent directories itself. */
    err = keep(err, close_inode(dir_ni));
    return keep(err, close_inode(ni));
}

/* Remove name `u` of inode ino from dir_ino (frees the inode on last link). */
static int unlink_ino(ntfsb_volume *v, uint64_t ino, uint64_t dir_ino, const uname_t *u)
{
    ntfs_inode *ni, *dir_ni;
    int err = open_inode(v, ino, &ni);
    if (err)
        return err;
    err = open_dir(v, dir_ino, &dir_ni);
    if (err) {
        ntfs_inode_close(ni);
        return err;
    }
    /* ntfs_delete() closes both inodes; it refuses (ENOTEMPTY) to remove the
     * last name of a non-empty directory. */
    if (ntfs_delete(v->vol, NULL, ni, dir_ni, u->s, u->len))
        return errno_or(EIO);
    return 0;
}

int ntfsb_link(ntfsb_volume *vol, uint64_t ino, uint64_t dir_ino, const char *name_utf8)
{
    if (!vol)
        return EINVAL;
    uname_t u;
    int err = make_uname(name_utf8, &u);
    if (err)
        return err;
    vlock(vol);
    err = check_writable_dir(vol, dir_ino);
    if (!err && is_metadata_ino(ino))
        err = EPERM;
    if (!err) {
        ntfs_inode *ni;
        err = open_inode(vol, ino, &ni);
        if (!err) {
            if (inode_is_dir(ni))
                err = EPERM; /* POSIX forbids hard links to directories */
            err = keep(err, close_inode(ni));
        }
    }
    if (!err) {
        uint64_t existing;
        err = lookup_ino(vol, dir_ino, &u, &existing);
        if (!err)
            err = EEXIST;
        else if (err == ENOENT)
            err = link_ino(vol, ino, dir_ino, &u);
    }
    vunlock(vol);
    return err;
}

int ntfsb_unlink(ntfsb_volume *vol, uint64_t dir_ino, const char *name_utf8)
{
    if (!vol)
        return EINVAL;
    uname_t u;
    int err = make_uname(name_utf8, &u);
    if (err)
        return err;
    vlock(vol);
    err = check_writable_dir(vol, dir_ino);
    uint64_t ino = 0;
    if (!err)
        err = lookup_ino(vol, dir_ino, &u, &ino);
    if (!err && is_metadata_ino(ino))
        err = EPERM;
    if (!err)
        err = unlink_ino(vol, ino, dir_ino, &u);
    vunlock(vol);
    return err;
}

static int ino_is_dir(ntfsb_volume *v, uint64_t ino, bool *is_dir)
{
    ntfs_inode *ni;
    int err = open_inode(v, ino, &ni);
    if (err)
        return err;
    *is_dir = inode_is_dir(ni);
    return close_inode(ni);
}

static int dir_is_empty(ntfsb_volume *v, uint64_t ino)
{
    ntfs_inode *ni;
    int err = open_inode(v, ino, &ni);
    if (err)
        return err;
    int r = ntfs_check_empty_dir(ni);
    err = r == 0 ? 0 : errno_or(EIO);
    return keep(err, close_inode(ni));
}

/* Is `anc` an ancestor of (or equal to) directory `ino`? */
static int is_ancestor(ntfsb_volume *v, uint64_t anc, uint64_t ino, bool *result)
{
    *result = false;
    for (unsigned i = 0; i < PARENT_WALK_MAX; i++) {
        if (ino == anc) {
            *result = true;
            return 0;
        }
        if (ino == FILE_root)
            return 0;
        int err = parent_of(v, ino, &ino);
        if (err)
            return err;
    }
    return ELOOP;
}

/*
 * Replace the existing destination `dst_ino` without a window in which
 * the destination name is missing on failure: the old destination is first
 * parked under a temporary name, so every step can be undone (same scheme
 * as ntfs-3g's ntfs_fuse_safe_rename()).
 */
static int rename_replace(ntfsb_volume *v, uint64_t src_ino, uint64_t src_dir, const uname_t *src,
                          uint64_t dst_ino, uint64_t dst_dir, const uname_t *dst)
{
    static unsigned long seq;
    char tmpname[64];
    uname_t tmp;
    snprintf(tmpname, sizeof(tmpname), ".ntfsb-rename-%llu-%lu",
             (unsigned long long)dst_ino, ++seq);
    int err = make_uname(tmpname, &tmp);
    if (err)
        return err;

    if ((err = link_ino(v, dst_ino, dst_dir, &tmp)))
        return err;
    if ((err = unlink_ino(v, dst_ino, dst_dir, dst))) {
        unlink_ino(v, dst_ino, dst_dir, &tmp);
        return err;
    }
    if ((err = link_ino(v, src_ino, dst_dir, dst))) {
        link_ino(v, dst_ino, dst_dir, dst);
        unlink_ino(v, dst_ino, dst_dir, &tmp);
        return err;
    }
    if ((err = unlink_ino(v, src_ino, src_dir, src))) {
        unlink_ino(v, src_ino, dst_dir, dst);
        link_ino(v, dst_ino, dst_dir, dst);
        unlink_ino(v, dst_ino, dst_dir, &tmp);
        return err;
    }
    /* Dropping the parked name frees the old destination. The rename has
     * already happened, so a failure here only leaves the temporary name
     * behind; it is reported in the log, not to the caller. */
    if (unlink_ino(v, dst_ino, dst_dir, &tmp))
        ntfs_log_error("rename: could not remove temporary name %s\n", tmpname);
    return 0;
}

int ntfsb_rename(ntfsb_volume *vol, uint64_t src_dir_ino, const char *src_name_utf8,
                 uint64_t dst_dir_ino, const char *dst_name_utf8)
{
    if (!vol)
        return EINVAL;
    uname_t src, dst;
    int err = make_uname(src_name_utf8, &src);
    if (!err)
        err = make_uname(dst_name_utf8, &dst);
    if (err)
        return err;

    vlock(vol);
    uint64_t src_ino = 0, dst_ino = 0;
    bool src_dir = false, dst_isdir = false, loop = false;

    err = check_writable_dir(vol, src_dir_ino);
    if (!err)
        err = check_writable_dir(vol, dst_dir_ino);
    if (!err)
        err = lookup_ino(vol, src_dir_ino, &src, &src_ino);
    if (!err && is_metadata_ino(src_ino))
        err = EPERM;
    if (!err)
        err = ino_is_dir(vol, src_ino, &src_dir);
    if (!err && src_dir) {
        /* Moving a directory below itself would detach a cycle. */
        err = is_ancestor(vol, src_ino, dst_dir_ino, &loop);
        if (!err && loop)
            err = EINVAL;
    }
    if (err)
        goto out;

    err = lookup_ino(vol, dst_dir_ino, &dst, &dst_ino);
    if (err == ENOENT) {
        /* Plain move: add the new name, then drop the old one. */
        err = link_ino(vol, src_ino, dst_dir_ino, &dst);
        if (!err) {
            err = unlink_ino(vol, src_ino, src_dir_ino, &src);
            if (err)
                unlink_ino(vol, src_ino, dst_dir_ino, &dst);
        }
        goto out;
    }
    if (err)
        goto out;

    if (dst_ino == src_ino) {
        /*
         * Both names already refer to the same inode. POSIX says do
         * nothing — except for a case-only rename ("foo" → "Foo") within
         * one directory, which a case-insensitive lookup reports as the
         * same entry.
         */
        bool case_only = src_dir_ino == dst_dir_ino &&
            !ntfs_names_are_equal(src.s, src.len, dst.s, dst.len, CASE_SENSITIVE,
                                  vol->vol->upcase, vol->vol->upcase_len) &&
            ntfs_names_are_equal(src.s, src.len, dst.s, dst.len, IGNORE_CASE,
                                 vol->vol->upcase, vol->vol->upcase_len);
        if (case_only) {
            err = link_ino(vol, src_ino, dst_dir_ino, &dst);
            if (!err) {
                err = unlink_ino(vol, src_ino, src_dir_ino, &src);
                if (err)
                    unlink_ino(vol, src_ino, dst_dir_ino, &dst);
            }
        }
        goto out;
    }

    if (is_metadata_ino(dst_ino)) {
        err = EPERM;
        goto out;
    }
    err = ino_is_dir(vol, dst_ino, &dst_isdir);
    if (!err && src_dir && !dst_isdir)
        err = ENOTDIR;
    if (!err && !src_dir && dst_isdir)
        err = EISDIR;
    if (!err && dst_isdir)
        err = dir_is_empty(vol, dst_ino);
    if (!err)
        err = rename_replace(vol, src_ino, src_dir_ino, &src, dst_ino, dst_dir_ino, &dst);
out:
    vunlock(vol);
    return err;
}

/* ------------------------------------------------------------------------ */
/* File data                                                                 */
/* ------------------------------------------------------------------------ */

static int open_data(ntfsb_volume *v, uint64_t ino, bool write, ntfs_inode **ni_out, ntfs_attr **na_out)
{
    if (write) {
        if (NVolReadOnly(v->vol))
            return EROFS;
        if (is_metadata_ino(ino))
            return EPERM;
    }
    ntfs_inode *ni;
    int err = open_inode(v, ino, &ni);
    if (err)
        return err;
    if (inode_is_dir(ni))
        err = EISDIR;
    else if (ni->flags & FILE_ATTR_ENCRYPTED)
        err = EACCES; /* EFS data needs the user's private key */
    else if (write && is_symlink_inode(v, ni))
        err = EINVAL;
    if (err) {
        ntfs_inode_close(ni);
        return err;
    }
    ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    if (!na) {
        err = errno_or(EIO);
        ntfs_inode_close(ni);
        return err;
    }
    *ni_out = ni;
    *na_out = na;
    return 0;
}

int ntfsb_read(ntfsb_volume *vol, uint64_t ino, void *buf, uint64_t len,
               int64_t offset, uint64_t *out_read)
{
    if (!vol || (!buf && len) || offset < 0)
        return EINVAL;
    vlock(vol);
    ntfs_inode *ni;
    ntfs_attr *na;
    uint64_t total = 0;
    int err = open_data(vol, ino, false, &ni, &na);
    if (!err) {
        if (offset < na->data_size) {
            uint64_t avail = (uint64_t)(na->data_size - offset);
            if (len > avail)
                len = avail;
            while (total < len) {
                s64 r = ntfs_attr_pread(na, offset + (s64)total, (s64)(len - total),
                                        (u8 *)buf + total);
                if (r < 0) {
                    err = errno_or(EIO);
                    break;
                }
                if (r == 0)
                    break;
                total += (uint64_t)r;
            }
        }
        /* atime is not updated: it would turn every read into an MFT write
         * (the ntfs-3g "noatime" behaviour). */
        ntfs_attr_close(na);
        err = keep(err, close_inode(ni));
    }
    if (out_read)
        *out_read = total;
    vunlock(vol);
    return err;
}

int ntfsb_write(ntfsb_volume *vol, uint64_t ino, const void *buf, uint64_t len,
                int64_t offset, uint64_t *out_written)
{
    if (!vol || (!buf && len) || offset < 0)
        return EINVAL;
    if (len > (uint64_t)(INT64_MAX - offset))
        return EFBIG;
    vlock(vol);
    ntfs_inode *ni;
    ntfs_attr *na;
    uint64_t total = 0;
    int err = open_data(vol, ino, true, &ni, &na);
    if (!err) {
        while (total < len) {
            s64 w = ntfs_attr_pwrite(na, offset + (s64)total, (s64)(len - total),
                                     (const u8 *)buf + total);
            if (w <= 0) {
                err = w < 0 ? errno_or(EIO) : EIO;
                break;
            }
            total += (uint64_t)w;
        }
        /* Compressed streams keep the last compression block uncompressed
         * until the attribute is "closed for writing". Since we do not keep
         * the attribute open between calls, finish it now. */
        if (total && (na->data_flags & ATTR_COMPRESSION_MASK) && ntfs_attr_pclose(na))
            err = keep(err, errno_or(EIO));
        if (total) {
            ni->flags |= FILE_ATTR_ARCHIVE;
            ntfs_inode_update_times(ni, NTFS_UPDATE_MCTIME);
        }
        ntfs_attr_close(na);
        err = keep(err, close_inode(ni));
    }
    if (out_written)
        *out_written = total;
    vunlock(vol);
    return err;
}

int ntfsb_truncate(ntfsb_volume *vol, uint64_t ino, uint64_t size)
{
    if (!vol)
        return EINVAL;
    vlock(vol);
    int err = NVolReadOnly(vol->vol) ? EROFS : is_metadata_ino(ino) ? EPERM : 0;
    if (!err) {
        ntfs_inode *ni;
        err = open_inode(vol, ino, &ni);
        if (!err) {
            err = truncate_open_inode(vol, ni, size);
            err = keep(err, close_inode(ni));
        }
    }
    vunlock(vol);
    return err;
}

/* ------------------------------------------------------------------------ */
/* Probe / mount / volume                                                    */
/* ------------------------------------------------------------------------ */

static bool io_valid(const ntfsb_io *io)
{
    return io && io->pread && io->block_size && !(io->block_size & (io->block_size - 1)) &&
           io->size_bytes >= io->block_size;
}

int ntfsb_probe(const ntfsb_io *io, ntfsb_probe_info *out)
{
    if (!io_valid(io) || !out)
        return EINVAL;
    ensure_log_handler();
    memset(out, 0, sizeof(*out));

    /* The probe never writes, whatever the host passed. */
    ntfsb_io ro = *io;
    ro.read_only = true;
    ro.pwrite = NULL;
    ntfsb_devio *dio = ntfsb_devio_new(&ro);
    if (!dio)
        return errno_or(ENOMEM);

    boot_info bi;
    int err = read_boot_sector(dio, &bi);
    if (err || !bi.is_ntfs) {
        ntfsb_devio_free(dio);
        return err;
    }
    out->is_ntfs = true;
    out->bytes_per_sector = bi.bytes_per_sector;
    out->cluster_size = bi.cluster_size;
    out->total_sectors = bi.total_sectors;
    out->serial = bi.serial;
    uuid_from_serial(bi.serial, out->uuid);

    /* Label and health need the metadata files: try a read-only mount. A
     * failure here still leaves a positive (if less detailed) probe. */
    struct ntfs_device *dev = ntfs_device_alloc("ntfsb-probe", 0, &ntfsb_device_ops, dio);
    if (dev) {
        ntfs_volume *vol = ntfs_device_mount(dev, NTFS_MNT_RDONLY);
        if (vol) {
            bool unclean, fast_startup;
            read_label(vol, out->label, sizeof(out->label));
            check_logfile(vol, &unclean, &fast_startup);
            out->dirty = (vol->flags & VOLUME_IS_DIRTY) || unclean;
            out->hibernated = fast_startup || is_hibernated(vol);
            ntfs_umount(vol, FALSE); /* frees dev */
        } else {
            ntfs_device_free(dev);
        }
    }
    ntfsb_devio_free(dio);
    return 0;
}

/* Delete hiberfil.sys from the root (the REMOVE_HIBER mount option). */
static int remove_hiberfile(ntfsb_volume *v)
{
    static const char name[] = "hiberfil.sys";
    uname_t u;
    int err = make_uname(name, &u);
    if (err)
        return err;
    /* Windows stores it as "hiberfil.sys" in the Win32 namespace, but match
     * it case-insensitively like Windows would. */
    bool cs = NVolCaseSensitive(v->vol);
    NVolClearCaseSensitive(v->vol);
    uint64_t ino;
    err = lookup_ino(v, FILE_root, &u, &ino);
    if (cs)
        NVolSetCaseSensitive(v->vol);
    if (err)
        return err == ENOENT ? 0 : err;
    return unlink_ino(v, ino, FILE_root, &u);
}

static ntfs_volume *device_mount(ntfsb_devio *dio, unsigned long flags, int *err)
{
    struct ntfs_device *dev = ntfs_device_alloc("ntfsb", 0, &ntfsb_device_ops, dio);
    if (!dev) {
        *err = errno_or(ENOMEM);
        return NULL;
    }
    ntfs_volume *vol = ntfs_device_mount(dev, (ntfs_mount_flags)flags);
    if (!vol) {
        *err = errno_or(EIO);
        ntfs_device_free(dev);
    }
    return vol;
}

int ntfsb_mount(const ntfsb_io *io, const ntfsb_mount_opts *opts, ntfsb_volume **out_vol)
{
    if (!io_valid(io) || !opts || !out_vol)
        return EINVAL;
    ensure_log_handler();
    *out_vol = NULL;

    ntfsb_volume *v = calloc(1, sizeof(*v));
    if (!v)
        return ENOMEM;
    v->opts = *opts;
    v->show_sys = (opts->flags & NTFSB_MOUNT_SHOW_SYS_FILES) != 0;
    bool rdonly = (opts->flags & NTFSB_MOUNT_RDONLY) || io->read_only;

    int err = 0;
    v->devio = ntfsb_devio_new(io);
    if (!v->devio) {
        err = errno_or(ENOMEM);
        goto fail;
    }
    boot_info bi;
    err = read_boot_sector(v->devio, &bi);
    if (!err && !bi.is_ntfs)
        err = EINVAL;
    if (err)
        goto fail;
    v->serial = bi.serial;

    unsigned long flags = NTFS_MNT_NONE;
    if (rdonly)
        flags |= NTFS_MNT_RDONLY;
    if (opts->flags & NTFSB_MOUNT_RECOVER)
        flags |= NTFS_MNT_RECOVER;
    if (opts->flags & NTFSB_MOUNT_REMOVE_HIBER)
        flags |= NTFS_MNT_IGNORE_HIBERFILE;

    v->vol = device_mount(v->devio, flags, &err);
    if (!v->vol) {
        /*
         * A read-write mount refuses hibernated volumes (EPERM), Fast
         * Startup caches (EPERM) and unclean journals (EOPNOTSUPP). The
         * contract reports all of them as EPERM, but only if the volume is
         * otherwise mountable — verify with a read-only attempt so that
         * genuine corruption keeps its own error.
         */
        if (!rdonly && (err == EPERM || err == EOPNOTSUPP)) {
            int ro_err = 0;
            ntfs_volume *ro = device_mount(v->devio, NTFS_MNT_RDONLY, &ro_err);
            if (ro) {
                ntfs_umount(ro, FALSE);
                err = EPERM;
            } else {
                err = ro_err;
            }
        }
        goto fail;
    }

    ntfs_create_lru_caches(v->vol);
    /* Metadata only on request; files with the HIDDEN attribute are always
     * listed (reported as NTFSB_FLAG_HIDDEN → UF_HIDDEN for Finder). */
    ntfs_set_shown_files(v->vol, v->show_sys, TRUE, FALSE);
    /* New files inherit compression from compressed directories. */
    NVolSetCompression(v->vol);
    if ((opts->flags & NTFSB_MOUNT_IGNORE_CASE) && ntfs_set_ignore_case(v->vol)) {
        err = errno_or(ENOMEM);
        goto fail_mounted;
    }
    if (ntfs_volume_get_free_space(v->vol)) {
        err = errno_or(EIO);
        goto fail_mounted;
    }
    if (pthread_mutex_init(&v->lock, NULL)) {
        err = ENOMEM;
        goto fail_mounted;
    }
    if (!NVolReadOnly(v->vol) && (opts->flags & NTFSB_MOUNT_REMOVE_HIBER) && is_hibernated(v->vol)) {
        err = remove_hiberfile(v);
        if (err) {
            pthread_mutex_destroy(&v->lock);
            goto fail_mounted;
        }
    }
    *out_vol = v;
    return 0;

fail_mounted:
    ntfs_umount(v->vol, TRUE);
fail:
    ntfsb_devio_free(v->devio);
    free(v);
    return err;
}

int ntfsb_unmount(ntfsb_volume *vol, bool force)
{
    if (!vol)
        return EINVAL;
    vlock(vol);
    /* ntfs_umount() syncs the system inodes and the device, closes it and
     * frees the volume even when it reports an error. */
    int err = ntfs_umount(vol->vol, force ? TRUE : FALSE) ? errno_or(EIO) : 0;
    vol->vol = NULL;
    vunlock(vol);
    pthread_mutex_destroy(&vol->lock);
    ntfsb_devio_free(vol->devio);
    free(vol);
    return err;
}

int ntfsb_sync(ntfsb_volume *vol)
{
    if (!vol)
        return EINVAL;
    vlock(vol);
    ntfs_volume *nv = vol->vol;
    int err = 0;
    if (!NVolReadOnly(nv)) {
        /*
         * File inodes are closed (and so written) at the end of every
         * bridge call. What can still be dirty in memory are the system
         * inodes libntfs-3g keeps open for the whole mount.
         */
        ntfs_inode *sys[] = { nv->mft_ni, nv->mftmirr_ni, nv->lcnbmp_ni, nv->vol_ni };
        for (size_t i = 0; i < sizeof(sys) / sizeof(sys[0]); i++)
            if (sys[i] && NInoDirty(sys[i]) && ntfs_inode_sync(sys[i]))
                err = keep(err, errno_or(EIO));
        if (nv->dev->d_ops->sync(nv->dev))
            err = keep(err, errno_or(EIO));
    }
    vunlock(vol);
    return err;
}

int ntfsb_volume_info_get(ntfsb_volume *vol, ntfsb_volume_info *out)
{
    if (!vol || !out)
        return EINVAL;
    vlock(vol);
    ntfs_volume *nv = vol->vol;
    memset(out, 0, sizeof(*out));
    int err = read_label(nv, out->label, sizeof(out->label));
    out->serial = vol->serial;
    uuid_from_serial(vol->serial, out->uuid);
    out->cluster_size = nv->cluster_size;
    out->sector_size = nv->sector_size;
    out->total_clusters = (uint64_t)nv->nr_clusters;
    s64 free_clusters = nv->free_clusters > 0 ? nv->free_clusters : 0;
    out->free_clusters = (uint64_t)free_clusters;
    /*
     * The MFT grows on demand, so free space is also room for new records
     * (the ntfs-3g statfs computation): records that fit in the free
     * clusters plus the free records already in the MFT bitmap.
     */
    int delta = nv->cluster_size_bits - nv->mft_record_size_bits;
    s64 records_in_free = delta >= 0 ? free_clusters << delta : free_clusters >> -delta;
    out->total_mft_records = (uint64_t)((nv->mftbmp_na->allocated_size << 3) + records_in_free);
    s64 free_records = records_in_free + (nv->free_mft_records > 0 ? nv->free_mft_records : 0);
    out->free_mft_records = (uint64_t)free_records;
    out->major_ver = nv->major_ver;
    out->minor_ver = nv->minor_ver;
    out->read_only = NVolReadOnly(nv);
    vunlock(vol);
    return err;
}

int ntfsb_set_label(ntfsb_volume *vol, const char *label_utf8)
{
    if (!vol || !label_utf8)
        return EINVAL;
    ntfschar label[LABEL_UNITS_MAX];
    size_t n;
    int err = utf8_to_utf16(label_utf8, label, LABEL_UNITS_MAX, &n);
    if (err)
        return err;
    vlock(vol);
    if (NVolReadOnly(vol->vol))
        err = EROFS;
    else if (ntfs_volume_rename(vol->vol, label, (int)n))
        err = errno_or(EIO);
    vunlock(vol);
    return err;
}

/* ------------------------------------------------------------------------ */
/* Misc                                                                      */
/* ------------------------------------------------------------------------ */

const char *ntfsb_strerror(int err)
{
    switch (err) {
    case 0:            return "Success";
    case EPERM:        return "The volume is hibernated, cached by Windows Fast Startup, or was not "
                              "shut down cleanly; it can only be mounted read-only (or the operation "
                              "touches NTFS metadata, which is not permitted)";
    case EROFS:        return "The NTFS volume is mounted read-only";
    case EIO:          return "Input/output error or NTFS metadata corruption (run chkdsk on Windows)";
    case EACCES:       return "The file is encrypted with EFS and cannot be accessed";
    case ENOTEMPTY:    return "Directory not empty";
    case ENAMETOOLONG: return "Name too long (NTFS allows 255 UTF-16 units per name, 32 for labels)";
    case EILSEQ:       return "Name is not valid UTF-8";
    case ENOSPC:       return "No space left on the NTFS volume";
    case ECANCELED:    return "Formatting was cancelled";
    case EOPNOTSUPP:   return "Operation not supported by NTFS";
    default:           return strerror(err);
    }
}

const char *ntfsb_libntfs_version(void)
{
    return NTFSB_LIBNTFS_VERSION;
}
