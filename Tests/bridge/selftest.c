/*
 * selftest.c — host-side (Linux/macOS) end-to-end test of the C bridge.
 *
 * Formats a disk image through ntfsb_format, then exercises every function
 * of NTFSBridge.h through ntfsb_io callbacks that read/write the image with
 * pread/pwrite. The callbacks abort on any request that is not aligned to
 * the advertised block size, which verifies the bridge's bounce layer.
 *
 * Usage: selftest <scratch-dir> [ntfsfix-binary]
 * Runs the whole suite for block sizes 512 and 4096.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "NTFSBridge.h"

#define IMAGE_SIZE (64ull * 1024 * 1024)
#define MANY_FILES 300

static int g_failures;
static int g_checks;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(cond)) {                                                        \
            g_failures++;                                                     \
            fprintf(stderr, "FAIL %s:%d: %s — ", __FILE__, __LINE__, #cond);  \
            fprintf(stderr, __VA_ARGS__);                                     \
            fputc('\n', stderr);                                              \
        }                                                                     \
    } while (0)

#define CHECK_RC(expr, want)                                                  \
    do {                                                                      \
        int rc_ = (expr);                                                     \
        CHECK(rc_ == (want), "%s returned %d (%s), want %d", #expr, rc_,      \
              ntfsb_strerror(rc_), (want));                                   \
    } while (0)

/* ------------------------------------------------------------------------ */
/* Image-backed ntfsb_io                                                     */
/* ------------------------------------------------------------------------ */

typedef struct image {
    int fd;
    uint32_t bs;
    uint64_t reads, writes, syncs;
} image;

static void check_aligned(const image *im, uint64_t count, int64_t offset, const char *op)
{
    if (count % im->bs || offset % im->bs) {
        fprintf(stderr, "FATAL: unaligned %s count=%" PRIu64 " offset=%" PRId64 " bs=%u\n",
                op, count, offset, im->bs);
        abort();
    }
}

static int64_t img_pread(void *ctx, void *buf, uint64_t count, int64_t offset)
{
    image *im = ctx;
    check_aligned(im, count, offset, "read");
    im->reads++;
    ssize_t r = pread(im->fd, buf, count, offset);
    return r < 0 ? -errno : r;
}

static int64_t img_pwrite(void *ctx, const void *buf, uint64_t count, int64_t offset)
{
    image *im = ctx;
    check_aligned(im, count, offset, "write");
    im->writes++;
    ssize_t r = pwrite(im->fd, buf, count, offset);
    return r < 0 ? -errno : r;
}

static int img_sync(void *ctx)
{
    image *im = ctx;
    im->syncs++;
    return fsync(im->fd) ? errno : 0;
}

static ntfsb_io make_io(image *im, bool ro)
{
    ntfsb_io io = {
        .ctx = im,
        .pread = img_pread,
        .pwrite = ro ? NULL : img_pwrite,
        .sync = img_sync,
        .size_bytes = IMAGE_SIZE,
        .block_size = im->bs,
        .read_only = ro,
    };
    return io;
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct progress_log {
    int calls;
    double last;
    double cancel_at; /* < 0: never cancel */
    bool monotonic;
} progress_log;

static int on_progress(void *ctx, double pct)
{
    progress_log *p = ctx;
    if (pct < p->last)
        p->monotonic = false;
    p->last = pct;
    p->calls++;
    return p->cancel_at >= 0 && pct >= p->cancel_at;
}

typedef struct entry {
    char name[NTFSB_MAX_NAME_UTF8];
    uint64_t ino;
    ntfsb_type type;
    uint64_t next;
} entry;

typedef struct listing {
    entry *e;
    size_t n, cap;
    size_t limit; /* stop after this many entries in one call (0 = never) */
    size_t taken; /* entries taken in the current call */
} listing;

static int collect(void *ctx, const char *name, uint64_t ino, ntfsb_type type, uint64_t next)
{
    listing *l = ctx;
    if (l->limit && l->taken == l->limit)
        return 1; /* "packer full": this entry is NOT consumed */
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->e = realloc(l->e, l->cap * sizeof(entry));
    }
    entry *e = &l->e[l->n++];
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->ino = ino;
    e->type = type;
    e->next = next;
    l->taken++;
    return 0;
}

/* Enumerate a directory, `per_call` entries per ntfsb_readdir call. */
static int list_dir(ntfsb_volume *v, uint64_t dir, size_t per_call, listing *out, int *calls)
{
    memset(out, 0, sizeof(*out));
    out->limit = per_call;
    uint64_t cookie = 0;
    bool eof = false;
    *calls = 0;
    while (!eof) {
        out->taken = 0;
        int rc = ntfsb_readdir(v, dir, cookie, collect, out, &eof);
        (*calls)++;
        if (rc)
            return rc;
        if (out->taken)
            cookie = out->e[out->n - 1].next;
        else if (!eof)
            return EPROTO; /* no progress */
        if (*calls > 100000)
            return ELOOP;
    }
    return 0;
}

static const entry *find_entry(const listing *l, const char *name)
{
    for (size_t i = 0; i < l->n; i++)
        if (strcmp(l->e[i].name, name) == 0)
            return &l->e[i];
    return NULL;
}

static size_t count_entry(const listing *l, const char *name)
{
    size_t c = 0;
    for (size_t i = 0; i < l->n; i++)
        c += strcmp(l->e[i].name, name) == 0;
    return c;
}

static uint64_t mk(ntfsb_volume *v, uint64_t dir, const char *name, ntfsb_type t)
{
    ntfsb_stat st;
    int rc = ntfsb_create(v, dir, name, t, t == NTFSB_TYPE_DIR ? 0755 : 0644, &st);
    CHECK(rc == 0, "create %s: %s", name, ntfsb_strerror(rc));
    return rc ? 0 : st.ino;
}

static void fill_pattern(uint8_t *p, size_t n, uint32_t seed)
{
    for (size_t i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        p[i] = (uint8_t)(seed >> 16);
    }
}

static uint64_t lookup_ino(ntfsb_volume *v, uint64_t dir, const char *name)
{
    ntfsb_stat st;
    return ntfsb_lookup(v, dir, name, &st) ? 0 : st.ino;
}

/* ------------------------------------------------------------------------ */
/* Test phases                                                               */
/* ------------------------------------------------------------------------ */

static const char *LABEL = "Tëst Vol";       /* non-ASCII on purpose */
static const char *LABEL2 = "Renamed ✓ 𝄞";    /* BMP + astral (surrogate pair) */
static const char *UNAME = "ünïcødé-日本語-😀.txt";

#define DATA_LEN 200003
#define DATA_OFF 777

static void test_format_and_probe(image *im, bool quick)
{
    ntfsb_io io = make_io(im, false);

    /* Cancelled format must report ECANCELED and leave the process usable. */
    progress_log cancel = { .cancel_at = 20.0, .monotonic = true };
    ntfsb_format_opts fo = { .label_utf8 = LABEL, .quick = false };
    CHECK_RC(ntfsb_format(&io, &fo, on_progress, &cancel), ECANCELED);

    /* Invalid parameters are rejected before mkntfs runs. */
    ntfsb_format_opts bad = { .cluster_size = 3000 };
    CHECK_RC(ntfsb_format(&io, &bad, NULL, NULL), EINVAL);
    ntfsb_format_opts longlabel = { .label_utf8 = "0123456789012345678901234567890123" };
    CHECK_RC(ntfsb_format(&io, &longlabel, NULL, NULL), ENAMETOOLONG);

    progress_log prog = { .cancel_at = -1, .monotonic = true };
    fo.quick = quick;
    CHECK_RC(ntfsb_format(&io, &fo, on_progress, &prog), 0);
    CHECK(prog.calls >= 2 && prog.last == 100.0 && prog.monotonic,
          "progress calls=%d last=%f monotonic=%d", prog.calls, prog.last, prog.monotonic);

    ntfsb_probe_info pi;
    CHECK_RC(ntfsb_probe(&io, &pi), 0);
    CHECK(pi.is_ntfs, "probe is_ntfs");
    CHECK(strcmp(pi.label, LABEL) == 0, "probe label '%s'", pi.label);
    CHECK(pi.bytes_per_sector == im->bs, "bytes_per_sector %u", pi.bytes_per_sector);
    CHECK(pi.cluster_size == 4096, "cluster_size %u", pi.cluster_size);
    CHECK(pi.total_sectors == IMAGE_SIZE / im->bs - 1, "total_sectors %" PRIu64, pi.total_sectors);
    CHECK(!pi.dirty && !pi.hibernated, "dirty=%d hibernated=%d", pi.dirty, pi.hibernated);
    CHECK((pi.uuid[6] >> 4) == 5 && (pi.uuid[8] & 0xC0) == 0x80, "uuid version/variant");
}

static void test_not_ntfs(const char *dir, uint32_t bs)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/zero-%u.img", dir, bs);
    image im = { .fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644), .bs = bs };
    CHECK(im.fd >= 0 && ftruncate(im.fd, 4 << 20) == 0, "zero image");
    ntfsb_io io = make_io(&im, true);
    io.size_bytes = 4 << 20;
    ntfsb_probe_info pi;
    CHECK_RC(ntfsb_probe(&io, &pi), 0);
    CHECK(!pi.is_ntfs, "zero image is not NTFS");
    ntfsb_volume *v = NULL;
    ntfsb_mount_opts mo = { 0 };
    CHECK_RC(ntfsb_mount(&io, &mo, &v), EINVAL);
    close(im.fd);
    unlink(path);
}

typedef struct expect {
    uint64_t dir1, file1, many, linkfile;
    struct timespec mtime, btime;
    uint8_t data[DATA_OFF + DATA_LEN];
} expect;

static void test_namespace_and_data(ntfsb_volume *v, expect *x)
{
    ntfsb_stat st, st2;

    /* Root. */
    CHECK_RC(ntfsb_getattr(v, NTFSB_ROOT_INO, &st), 0);
    CHECK(st.type == NTFSB_TYPE_DIR && (st.mode & S_IFMT) == S_IFDIR, "root is a dir");
    CHECK(st.parent_ino == NTFSB_ROOT_INO, "root parent %" PRIu64, st.parent_ino);
    CHECK(st.uid == 501 && st.gid == 20, "uid/gid from mount options");
    CHECK((st.mode & 0777) == 0755, "dir mode %o", st.mode & 0777);

    /* System files are hidden. */
    CHECK_RC(ntfsb_lookup(v, NTFSB_ROOT_INO, "$MFT", &st), ENOENT);
    CHECK_RC(ntfsb_lookup(v, NTFSB_ROOT_INO, ".", &st), 0);
    CHECK(st.ino == NTFSB_ROOT_INO, "lookup . in root");
    CHECK_RC(ntfsb_getattr(v, 999999, &st), ENOENT);

    /* Directories and files. */
    x->dir1 = mk(v, NTFSB_ROOT_INO, "dir1", NTFSB_TYPE_DIR);
    CHECK_RC(ntfsb_create(v, NTFSB_ROOT_INO, "dir1", NTFSB_TYPE_FILE, 0644, NULL), EEXIST);
    CHECK_RC(ntfsb_create(v, NTFSB_ROOT_INO, "a/b", NTFSB_TYPE_FILE, 0644, NULL), EINVAL);
    CHECK_RC(ntfsb_create(v, NTFSB_ROOT_INO, "..", NTFSB_TYPE_FILE, 0644, NULL), EINVAL);
    char longname[300];
    memset(longname, 'x', 256);
    longname[256] = 0;
    CHECK_RC(ntfsb_create(v, NTFSB_ROOT_INO, longname, NTFSB_TYPE_FILE, 0644, NULL), ENAMETOOLONG);
    CHECK_RC(ntfsb_create(v, NTFSB_ROOT_INO, "bad\xff", NTFSB_TYPE_FILE, 0644, NULL), EILSEQ);
    CHECK_RC(ntfsb_create(v, x->dir1, "x", NTFSB_TYPE_SYMLINK, 0644, NULL), EINVAL);

    x->file1 = mk(v, x->dir1, UNAME, NTFSB_TYPE_FILE);
    CHECK_RC(ntfsb_lookup(v, x->dir1, UNAME, &st), 0);
    CHECK(st.ino == x->file1 && st.type == NTFSB_TYPE_FILE && st.parent_ino == x->dir1 &&
          st.nlink == 1 && st.size == 0, "lookup unicode file");
    CHECK_RC(ntfsb_lookup(v, x->dir1, "..", &st), 0);
    CHECK(st.ino == NTFSB_ROOT_INO, "lookup .. of dir1");
    CHECK_RC(ntfsb_lookup(v, x->file1, "x", &st), ENOTDIR);

    /* Unaligned writes in odd-sized chunks, then unaligned reads. */
    fill_pattern(x->data + DATA_OFF, DATA_LEN, 42);
    memset(x->data, 0, DATA_OFF);
    size_t chunks[] = { 1, 510, 513, 4095, 4097, 65537, 3, 77777 };
    uint64_t off = DATA_OFF;
    size_t ci = 0;
    while (off < DATA_OFF + DATA_LEN) {
        size_t n = chunks[ci++ % (sizeof(chunks) / sizeof(chunks[0]))];
        if (off + n > DATA_OFF + DATA_LEN)
            n = DATA_OFF + DATA_LEN - off;
        uint64_t w = 0;
        CHECK_RC(ntfsb_write(v, x->file1, x->data + off, n, (int64_t)off, &w), 0);
        CHECK(w == n, "short write %" PRIu64 "/%zu", w, n);
        off += n;
    }
    CHECK_RC(ntfsb_getattr(v, x->file1, &st), 0);
    CHECK(st.size == DATA_OFF + DATA_LEN, "size after write %" PRIu64, st.size);
    CHECK(st.flags & NTFSB_FLAG_ARCHIVE, "archive flag set by write");

    static uint8_t buf[DATA_OFF + DATA_LEN + 4096];
    uint64_t r = 0;
    CHECK_RC(ntfsb_read(v, x->file1, buf, sizeof(buf), 0, &r), 0);
    CHECK(r == DATA_OFF + DATA_LEN, "read size %" PRIu64, r);
    CHECK(memcmp(buf, x->data, DATA_OFF + DATA_LEN) == 0, "full read matches");
    for (uint64_t o = 1; o < DATA_OFF + DATA_LEN; o += 9973) {
        size_t n = 1 + (o % 7001);
        CHECK_RC(ntfsb_read(v, x->file1, buf, n, (int64_t)o, &r), 0);
        size_t want = o + n > DATA_OFF + DATA_LEN ? DATA_OFF + DATA_LEN - o : n;
        CHECK(r == want && memcmp(buf, x->data + o, want) == 0, "read at %" PRIu64, o);
    }
    CHECK_RC(ntfsb_read(v, x->file1, buf, 10, DATA_OFF + DATA_LEN + 5, &r), 0);
    CHECK(r == 0, "read past EOF returns 0");
    CHECK_RC(ntfsb_read(v, x->dir1, buf, 10, 0, &r), EISDIR);

    /* Small resident file rewritten in place. */
    uint64_t small = mk(v, x->dir1, "small", NTFSB_TYPE_FILE);
    uint64_t w;
    CHECK_RC(ntfsb_write(v, small, "hello", 5, 0, &w), 0);
    CHECK_RC(ntfsb_write(v, small, "J", 1, 0, &w), 0);
    CHECK_RC(ntfsb_read(v, small, buf, 100, 0, &r), 0);
    CHECK(r == 5 && memcmp(buf, "Jello", 5) == 0, "small file content");

    /* truncate: shrink, grow (zero-filled). */
    CHECK_RC(ntfsb_truncate(v, small, 2), 0);
    CHECK_RC(ntfsb_truncate(v, small, 10000), 0);
    CHECK_RC(ntfsb_read(v, small, buf, 20000, 0, &r), 0);
    bool zeros = r == 10000 && buf[0] == 'J' && buf[1] == 'e';
    for (size_t i = 2; zeros && i < 10000; i++)
        zeros = buf[i] == 0;
    CHECK(zeros, "truncate grow zero fills (r=%" PRIu64 ")", r);
    CHECK_RC(ntfsb_truncate(v, x->dir1, 0), EISDIR);

    /* setattr: times, flags, mode. */
    ntfsb_setattr_req req = { .mask = NTFSB_SET_MTIME | NTFSB_SET_ATIME | NTFSB_SET_BTIME };
    req.mtime = (struct timespec){ 1234567890, 123456700 };
    req.atime = (struct timespec){ 1300000000, 0 };
    req.btime = (struct timespec){ 1000000000, 500 };
    CHECK_RC(ntfsb_setattr(v, x->file1, &req, &st), 0);
    CHECK(st.mtime.tv_sec == 1234567890 && st.mtime.tv_nsec == 123456700, "mtime set");
    CHECK(st.atime.tv_sec == 1300000000 && st.btime.tv_sec == 1000000000 &&
          st.btime.tv_nsec == 500, "atime/btime set");
    x->mtime = req.mtime;
    x->btime = req.btime;

    req = (ntfsb_setattr_req){ .mask = NTFSB_SET_FLAGS, .flags = NTFSB_FLAG_HIDDEN };
    CHECK_RC(ntfsb_setattr(v, small, &req, &st), 0);
    CHECK((st.flags & (NTFSB_FLAG_HIDDEN | NTFSB_FLAG_ARCHIVE)) == NTFSB_FLAG_HIDDEN,
          "flags %#x", st.flags);
    req = (ntfsb_setattr_req){ .mask = NTFSB_SET_MODE | NTFSB_SET_UID, .mode = 0444, .uid = 0 };
    CHECK_RC(ntfsb_setattr(v, small, &req, &st), 0);
    CHECK((st.flags & NTFSB_FLAG_READONLY) && (st.mode & 0222) == 0, "readonly via mode");
    req = (ntfsb_setattr_req){ .mask = NTFSB_SET_SIZE, .size = 3 };
    CHECK_RC(ntfsb_setattr(v, small, &req, &st), 0);
    CHECK(st.size == 3, "setattr size");
    req = (ntfsb_setattr_req){ .mask = NTFSB_SET_MODE, .mode = 0644 };
    CHECK_RC(ntfsb_setattr(v, small, &req, &st), 0);
    CHECK(!(st.flags & NTFSB_FLAG_READONLY) && (st.mode & 0777) == 0644, "writable again");
    req = (ntfsb_setattr_req){ .mask = NTFSB_SET_MTIME };
    CHECK_RC(ntfsb_setattr(v, 0, &req, NULL), EPERM);

    /* Hard links. */
    CHECK_RC(ntfsb_link(v, x->file1, NTFSB_ROOT_INO, "hardlink"), 0);
    CHECK_RC(ntfsb_link(v, x->file1, NTFSB_ROOT_INO, "hardlink"), EEXIST);
    CHECK_RC(ntfsb_link(v, x->dir1, NTFSB_ROOT_INO, "dirlink"), EPERM);
    CHECK_RC(ntfsb_getattr(v, x->file1, &st), 0);
    CHECK(st.nlink == 2, "nlink after link %u", st.nlink);
    CHECK_RC(ntfsb_lookup(v, NTFSB_ROOT_INO, "hardlink", &st2), 0);
    CHECK(st2.ino == x->file1, "hardlink resolves to file1");
    x->linkfile = x->file1;

    /* Symlinks. */
    CHECK_RC(ntfsb_symlink(v, x->dir1, "rel", UNAME, &st), 0);          /* native, file */
    CHECK(st.type == NTFSB_TYPE_SYMLINK && st.size == strlen(UNAME) &&
          (st.mode & S_IFMT) == S_IFLNK, "relative symlink stat");
    CHECK_RC(ntfsb_symlink(v, x->dir1, "updir", "../dir1", &st), 0);    /* native, dir */
    CHECK(st.type == NTFSB_TYPE_SYMLINK, "dir symlink type");
    CHECK_RC(ntfsb_symlink(v, x->dir1, "abs", "/usr/bin/env", &st), 0); /* WSL */
    CHECK_RC(ntfsb_symlink(v, x->dir1, "dangling", "nowhere/at all", &st), 0);
    CHECK_RC(ntfsb_symlink(v, x->dir1, "rel", "x", &st), EEXIST);
    const char *links[][2] = {
        { "rel", NULL }, { "updir", "../dir1" }, { "abs", "/usr/bin/env" },
        { "dangling", "nowhere/at all" },
    };
    links[0][1] = UNAME;
    for (size_t i = 0; i < 4; i++) {
        char t[1024];
        size_t len = 0;
        CHECK_RC(ntfsb_lookup(v, x->dir1, links[i][0], &st), 0);
        CHECK(st.type == NTFSB_TYPE_SYMLINK, "%s is symlink", links[i][0]);
        CHECK_RC(ntfsb_readlink(v, st.ino, t, sizeof(t), &len), 0);
        CHECK(strcmp(t, links[i][1]) == 0 && len == strlen(links[i][1]),
              "readlink %s = '%s'", links[i][0], t);
        CHECK_RC(ntfsb_readlink(v, st.ino, t, 3, &len), ERANGE);
    }
    char t[64];
    CHECK_RC(ntfsb_readlink(v, x->file1, t, sizeof(t), NULL), EINVAL);
    CHECK_RC(ntfsb_lookup(v, x->dir1, "rel", &st), 0);
    CHECK_RC(ntfsb_write(v, st.ino, "x", 1, 0, &w), EINVAL);

    /* unlink / rmdir. */
    CHECK_RC(ntfsb_unlink(v, NTFSB_ROOT_INO, "dir1"), ENOTEMPTY);
    uint64_t empty = mk(v, NTFSB_ROOT_INO, "emptydir", NTFSB_TYPE_DIR);
    CHECK_RC(ntfsb_unlink(v, NTFSB_ROOT_INO, "emptydir"), 0);
    CHECK_RC(ntfsb_getattr(v, empty, &st), ENOENT);
    CHECK_RC(ntfsb_unlink(v, NTFSB_ROOT_INO, "emptydir"), ENOENT);
    CHECK_RC(ntfsb_unlink(v, NTFSB_ROOT_INO, "$MFT"), EPERM);
    CHECK_RC(ntfsb_unlink(v, x->dir1, "dangling"), 0);
}

static void test_rename(ntfsb_volume *v, expect *x)
{
    ntfsb_stat st;
    uint64_t w;
    uint64_t r1 = mk(v, NTFSB_ROOT_INO, "ren", NTFSB_TYPE_DIR);
    uint64_t sub = mk(v, r1, "sub", NTFSB_TYPE_DIR);
    uint64_t a = mk(v, r1, "a", NTFSB_TYPE_FILE);
    uint64_t b = mk(v, r1, "b", NTFSB_TYPE_FILE);
    ntfsb_write(v, a, "AAAA", 4, 0, &w);
    ntfsb_write(v, b, "BB", 2, 0, &w);

    /* Plain rename within a dir and across dirs. */
    CHECK_RC(ntfsb_rename(v, r1, "a", r1, "a2"), 0);
    CHECK(lookup_ino(v, r1, "a") == 0 && lookup_ino(v, r1, "a2") == a, "rename a→a2");
    CHECK_RC(ntfsb_rename(v, r1, "a2", sub, "a3"), 0);
    CHECK(lookup_ino(v, sub, "a3") == a, "move into sub");
    CHECK_RC(ntfsb_getattr(v, a, &st), 0);
    CHECK(st.parent_ino == sub && st.nlink == 1, "parent after move");

    /* Replace an existing file: b's inode disappears, content is a's. */
    CHECK_RC(ntfsb_rename(v, sub, "a3", r1, "b"), 0);
    CHECK(lookup_ino(v, r1, "b") == a && lookup_ino(v, sub, "a3") == 0, "replace target");
    CHECK_RC(ntfsb_getattr(v, b, &st), ENOENT);
    char buf[16];
    uint64_t r = 0;
    CHECK_RC(ntfsb_read(v, a, buf, sizeof(buf), 0, &r), 0);
    CHECK(r == 4 && memcmp(buf, "AAAA", 4) == 0, "replaced content");
    listing l;
    int calls;
    CHECK_RC(list_dir(v, r1, 0, &l, &calls), 0);
    CHECK(l.n == 4, "no temp names left in ren/ (%zu entries)", l.n); /* . .. sub b */
    free(l.e);

    /* Directory rename, into its own subtree, over files/dirs. */
    uint64_t d2 = mk(v, r1, "d2", NTFSB_TYPE_DIR);
    mk(v, d2, "inner", NTFSB_TYPE_FILE);
    CHECK_RC(ntfsb_rename(v, r1, "d2", d2, "self"), EINVAL);
    CHECK_RC(ntfsb_rename(v, NTFSB_ROOT_INO, "ren", sub, "loop"), EINVAL);
    CHECK_RC(ntfsb_rename(v, r1, "b", r1, "d2"), EISDIR);
    CHECK_RC(ntfsb_rename(v, r1, "d2", r1, "b"), ENOTDIR);
    CHECK_RC(ntfsb_rename(v, r1, "sub", r1, "d2"), ENOTEMPTY);
    uint64_t e = mk(v, r1, "emptytarget", NTFSB_TYPE_DIR);
    CHECK_RC(ntfsb_rename(v, r1, "d2", r1, "emptytarget"), 0);
    CHECK(lookup_ino(v, r1, "emptytarget") == d2, "dir replaced empty dir");
    CHECK_RC(ntfsb_getattr(v, e, &st), ENOENT);
    CHECK(lookup_ino(v, d2, "inner") != 0, "dir contents moved along");
    CHECK_RC(ntfsb_rename(v, r1, "emptytarget", sub, "moved"), 0);
    CHECK_RC(ntfsb_lookup(v, d2, "..", &st), 0);
    CHECK(st.ino == sub, ".. follows the move");
    CHECK_RC(ntfsb_rename(v, r1, "nonexistent", r1, "zz"), ENOENT);

    /* Rename onto a hard link of the same inode: no-op. */
    CHECK_RC(ntfsb_rename(v, NTFSB_ROOT_INO, "hardlink", x->dir1, UNAME), 0);
    CHECK(lookup_ino(v, NTFSB_ROOT_INO, "hardlink") == x->file1, "same-inode rename no-op");
    /* Unlink one link: nlink back to 1. */
    CHECK_RC(ntfsb_unlink(v, NTFSB_ROOT_INO, "hardlink"), 0);
    CHECK_RC(ntfsb_getattr(v, x->file1, &st), 0);
    CHECK(st.nlink == 1, "nlink after unlink %u", st.nlink);
}

/*
 * 300 files force the directory index out of $INDEX_ROOT into
 * $INDEX_ALLOCATION blocks. Enumerate with every possible stop point (one
 * entry per call, i.e. resuming from every cookie) and in larger batches.
 */
static void test_readdir(ntfsb_volume *v, expect *x)
{
    x->many = mk(v, NTFSB_ROOT_INO, "many", NTFSB_TYPE_DIR);
    char name[64];
    for (int i = 0; i < MANY_FILES; i++) {
        snprintf(name, sizeof(name), "file-%03d-ü-%s", i, (i % 3) ? "long name for index" : "s");
        mk(v, x->many, name, i % 10 == 0 ? NTFSB_TYPE_DIR : NTFSB_TYPE_FILE);
    }
    size_t batches[] = { 1, 2, 7, 128, 0 };
    for (size_t bi = 0; bi < sizeof(batches) / sizeof(batches[0]); bi++) {
        listing l;
        int calls;
        CHECK_RC(list_dir(v, x->many, batches[bi], &l, &calls), 0);
        CHECK(l.n == MANY_FILES + 2, "batch %zu: %zu entries", batches[bi], l.n);
        CHECK(l.n >= 2 && strcmp(l.e[0].name, ".") == 0 && l.e[0].next == 1 &&
              strcmp(l.e[1].name, "..") == 0 && l.e[1].next == 2, ". and .. first");
        CHECK(l.n >= 2 && l.e[0].ino == x->many && l.e[1].ino == NTFSB_ROOT_INO, "./.. inodes");
        for (int i = 0; i < MANY_FILES; i++) {
            snprintf(name, sizeof(name), "file-%03d-ü-%s", i, (i % 3) ? "long name for index" : "s");
            size_t c = count_entry(&l, name);
            CHECK(c == 1, "batch %zu: %s seen %zu times", batches[bi], name, c);
            const entry *e = find_entry(&l, name);
            CHECK(e && e->type == (i % 10 == 0 ? NTFSB_TYPE_DIR : NTFSB_TYPE_FILE), "type of %s", name);
        }
        for (size_t i = 1; i < l.n; i++)
            CHECK(l.e[i].next > l.e[i - 1].next, "cookies increase");
        if (batches[bi] == 1)
            CHECK(calls == MANY_FILES + 2, "one entry per call: %d calls", calls);
        free(l.e);
    }
    /* Resume from an arbitrary middle cookie gives exactly the tail. */
    listing all;
    int calls;
    CHECK_RC(list_dir(v, x->many, 0, &all, &calls), 0);
    for (size_t k = 0; k < all.n; k += 37) {
        listing tail = { 0 };
        bool eof = false;
        CHECK_RC(ntfsb_readdir(v, x->many, all.e[k].next, collect, &tail, &eof), 0);
        CHECK(eof && tail.n == all.n - k - 1, "resume after #%zu: %zu", k, tail.n);
        for (size_t i = 0; i < tail.n; i++)
            CHECK(strcmp(tail.e[i].name, all.e[k + 1 + i].name) == 0, "tail order");
        free(tail.e);
    }
    /* EOF cookie and a bogus cookie. */
    listing none = { 0 };
    bool eof = false;
    CHECK_RC(ntfsb_readdir(v, x->many, all.e[all.n - 1].next, collect, &none, &eof), 0);
    CHECK(eof && none.n == 0, "resume at end");
    CHECK_RC(ntfsb_readdir(v, x->many, UINT64_C(1) << 40, collect, &none, &eof), EINVAL);
    CHECK_RC(ntfsb_readdir(v, x->dir1 + 0, 0, collect, &none, NULL), 0);
    free(none.e);
    free(all.e);
    CHECK_RC(ntfsb_readdir(v, x->file1, 0, collect, &none, NULL), ENOTDIR);

    /* Root listing hides $MFT & co. and shows our entries. */
    listing root;
    CHECK_RC(list_dir(v, NTFSB_ROOT_INO, 3, &root, &calls), 0);
    CHECK(!find_entry(&root, "$MFT") && !find_entry(&root, "$Extend"), "metadata hidden");
    CHECK(find_entry(&root, "dir1") && find_entry(&root, "many"), "root entries");
    free(root.e);
}

static void test_volume(ntfsb_volume *v)
{
    ntfsb_volume_info vi;
    CHECK_RC(ntfsb_volume_info_get(v, &vi), 0);
    CHECK(strcmp(vi.label, LABEL) == 0, "label '%s'", vi.label);
    CHECK(vi.cluster_size == 4096 && vi.total_clusters > 0 && vi.free_clusters < vi.total_clusters,
          "clusters %" PRIu64 "/%" PRIu64, vi.free_clusters, vi.total_clusters);
    CHECK(vi.free_mft_records > 0 && vi.free_mft_records < vi.total_mft_records, "mft records");
    CHECK(vi.major_ver == 3 && vi.minor_ver == 1 && !vi.read_only, "version / rw");
    CHECK_RC(ntfsb_set_label(v, "012345678901234567890123456789012"), ENAMETOOLONG);
    CHECK_RC(ntfsb_set_label(v, LABEL2), 0);
    CHECK_RC(ntfsb_volume_info_get(v, &vi), 0);
    CHECK(strcmp(vi.label, LABEL2) == 0, "label after set '%s'", vi.label);
    CHECK_RC(ntfsb_sync(v), 0);
}

static void test_persistence(ntfsb_volume *v, const expect *x, bool ro)
{
    ntfsb_stat st;
    ntfsb_volume_info vi;
    CHECK_RC(ntfsb_volume_info_get(v, &vi), 0);
    CHECK(strcmp(vi.label, LABEL2) == 0, "persisted label '%s'", vi.label);
    CHECK(vi.read_only == ro, "read_only=%d", vi.read_only);
    CHECK_RC(ntfsb_lookup(v, x->dir1, UNAME, &st), 0);
    CHECK(st.ino == x->file1 && st.size == DATA_OFF + DATA_LEN, "persisted file");
    CHECK(st.mtime.tv_sec == x->mtime.tv_sec && st.mtime.tv_nsec == x->mtime.tv_nsec &&
          st.btime.tv_sec == x->btime.tv_sec, "persisted times");
    static uint8_t buf[DATA_OFF + DATA_LEN];
    uint64_t r;
    CHECK_RC(ntfsb_read(v, x->file1, buf, sizeof(buf), 0, &r), 0);
    CHECK(r == sizeof(buf) && memcmp(buf, x->data, sizeof(buf)) == 0, "persisted data");
    char t[256];
    CHECK_RC(ntfsb_lookup(v, x->dir1, "abs", &st), 0);
    CHECK_RC(ntfsb_readlink(v, st.ino, t, sizeof(t), NULL), 0);
    CHECK(strcmp(t, "/usr/bin/env") == 0, "persisted symlink '%s'", t);
    listing l;
    int calls;
    CHECK_RC(list_dir(v, x->many, 50, &l, &calls), 0);
    CHECK(l.n == MANY_FILES + 2, "persisted listing %zu", l.n);
    free(l.e);
    uint64_t w;
    if (ro) {
        CHECK_RC(ntfsb_write(v, x->file1, "z", 1, 0, &w), EROFS);
        CHECK_RC(ntfsb_create(v, NTFSB_ROOT_INO, "nope", NTFSB_TYPE_FILE, 0644, NULL), EROFS);
        CHECK_RC(ntfsb_set_label(v, "x"), EROFS);
        CHECK_RC(ntfsb_sync(v), 0);
    }
}

static void test_ignore_case(ntfsb_volume *v, const expect *x)
{
    ntfsb_stat st;
    CHECK_RC(ntfsb_lookup(v, NTFSB_ROOT_INO, "DIR1", &st), 0);
    CHECK(st.ino == x->dir1, "case-insensitive lookup");
    mk(v, x->dir1, "case.txt", NTFSB_TYPE_FILE);
    CHECK_RC(ntfsb_create(v, x->dir1, "CASE.TXT", NTFSB_TYPE_FILE, 0644, NULL), EEXIST);
    CHECK_RC(ntfsb_rename(v, x->dir1, "case.txt", x->dir1, "Case.TXT"), 0);
    listing l;
    int calls;
    CHECK_RC(list_dir(v, x->dir1, 0, &l, &calls), 0);
    CHECK(find_entry(&l, "Case.TXT") && !find_entry(&l, "case.txt"), "case-only rename, case preserved");
    CHECK(find_entry(&l, UNAME) != NULL, "names not lowercased in listing");
    free(l.e);
}

/* A hiberfil.sys whose header says "hibr" is what Windows leaves behind. */
static void test_hibernation(const ntfsb_io *io, ntfsb_mount_opts mo)
{
    ntfsb_volume *v = NULL;
    mo.flags = 0;
    CHECK_RC(ntfsb_mount(io, &mo, &v), 0);
    if (!v)
        return;
    static uint8_t hdr[4096];
    memcpy(hdr, "hibr", 4);
    ntfsb_stat st;
    uint64_t w;
    CHECK_RC(ntfsb_create(v, NTFSB_ROOT_INO, "hiberfil.sys", NTFSB_TYPE_FILE, 0644, &st), 0);
    CHECK_RC(ntfsb_write(v, st.ino, hdr, sizeof(hdr), 0, &w), 0);
    CHECK_RC(ntfsb_unmount(v, false), 0);

    ntfsb_probe_info pi;
    CHECK_RC(ntfsb_probe(io, &pi), 0);
    CHECK(pi.is_ntfs && pi.hibernated, "probe detects hibernation");
    v = NULL;
    CHECK_RC(ntfsb_mount(io, &mo, &v), EPERM);
    CHECK(v == NULL, "no volume on EPERM");
    mo.flags = NTFSB_MOUNT_RDONLY;
    CHECK_RC(ntfsb_mount(io, &mo, &v), 0);
    if (v)
        CHECK_RC(ntfsb_unmount(v, false), 0);
    v = NULL;
    mo.flags = NTFSB_MOUNT_REMOVE_HIBER;
    CHECK_RC(ntfsb_mount(io, &mo, &v), 0);
    if (v) {
        CHECK_RC(ntfsb_lookup(v, NTFSB_ROOT_INO, "hiberfil.sys", &st), ENOENT);
        CHECK_RC(ntfsb_unmount(v, false), 0);
    }
    CHECK_RC(ntfsb_probe(io, &pi), 0);
    CHECK(!pi.hibernated, "hibernation cleared");
}

static int run_ntfsfix(const char *ntfsfix, const char *img)
{
    pid_t pid = fork();
    if (pid == 0) {
        execl(ntfsfix, ntfsfix, "-n", img, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void run_suite(const char *dir, uint32_t bs, const char *ntfsfix)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/ntfs-%u.img", dir, bs);
    image im = { .fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644), .bs = bs };
    if (im.fd < 0 || ftruncate(im.fd, (off_t)IMAGE_SIZE)) {
        perror(path);
        exit(2);
    }
    printf("== block size %u: %s\n", bs, path);

    test_not_ntfs(dir, bs);
    test_format_and_probe(&im, bs == 4096);

    static expect x;
    memset(&x, 0, sizeof(x));
    ntfsb_io io = make_io(&im, false);
    ntfsb_mount_opts mo = { .uid = 501, .gid = 20, .fmask = 022, .dmask = 022 };
    ntfsb_volume *v = NULL;
    CHECK_RC(ntfsb_mount(&io, &mo, &v), 0);
    if (!v)
        exit(1);
    test_namespace_and_data(v, &x);
    test_rename(v, &x);
    test_readdir(v, &x);
    test_volume(v);
    CHECK_RC(ntfsb_unmount(v, false), 0);

    /* Remount read-write and check that everything persisted. */
    v = NULL;
    CHECK_RC(ntfsb_mount(&io, &mo, &v), 0);
    if (v) {
        test_persistence(v, &x, false);
        CHECK_RC(ntfsb_unmount(v, false), 0);
    }

    /* Case-insensitive mount. */
    mo.flags = NTFSB_MOUNT_IGNORE_CASE;
    v = NULL;
    CHECK_RC(ntfsb_mount(&io, &mo, &v), 0);
    if (v) {
        test_ignore_case(v, &x);
        CHECK_RC(ntfsb_unmount(v, true), 0);
    }

    /* Read-only via the io and via the flag; system files visible on request. */
    ntfsb_io roio = make_io(&im, true);
    mo.flags = NTFSB_MOUNT_SHOW_SYS_FILES;
    v = NULL;
    CHECK_RC(ntfsb_mount(&roio, &mo, &v), 0);
    if (v) {
        test_persistence(v, &x, true);
        ntfsb_stat st;
        CHECK_RC(ntfsb_lookup(v, NTFSB_ROOT_INO, "$MFT", &st), 0);
        CHECK(st.ino == 0, "$MFT visible with SHOW_SYS_FILES");
        CHECK_RC(ntfsb_unmount(v, false), 0);
    }
    mo.flags = NTFSB_MOUNT_RDONLY;
    v = NULL;
    CHECK_RC(ntfsb_mount(&io, &mo, &v), 0);
    if (v) {
        uint64_t w;
        CHECK_RC(ntfsb_write(v, x.file1, "z", 1, 0, &w), EROFS);
        CHECK_RC(ntfsb_unmount(v, false), 0);
    }

    mo.flags = 0;
    test_hibernation(&io, mo);

    ntfsb_probe_info pi;
    CHECK_RC(ntfsb_probe(&io, &pi), 0);
    CHECK(pi.is_ntfs && !pi.dirty && strcmp(pi.label, LABEL2) == 0, "final probe");
    printf("   io: %" PRIu64 " reads, %" PRIu64 " writes, %" PRIu64 " syncs (all block-aligned)\n",
           im.reads, im.writes, im.syncs);
    close(im.fd);

    if (ntfsfix && *ntfsfix) {
        int rc = run_ntfsfix(ntfsfix, path);
        CHECK(rc == 0, "ntfsfix -n exit status %d", rc);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <scratch-dir> [ntfsfix]\n", argv[0]);
        return 2;
    }
    printf("libntfs-3g %s\n", ntfsb_libntfs_version());
    run_suite(argv[1], 512, argc > 2 ? argv[2] : NULL);
    run_suite(argv[1], 4096, argc > 2 ? argv[2] : NULL);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    puts(g_failures ? "SELFTEST FAILED" : "SELFTEST PASSED");
    return g_failures ? 1 : 0;
}
