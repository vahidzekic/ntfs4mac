/*
 * mkntfs_glue.c — ntfsb_format(): run the compiled-in mkntfs on an ntfsb_io.
 *
 * mkntfs keeps all of its state in globals, installs its own log handler and
 * calls setlocale(), so formats are serialised process-wide and the global
 * state that mkntfs leaves behind is restored afterwards. mkntfs never calls
 * exit(); every failure path returns from main() through mkntfs_cleanup().
 *
 * libntfs-3g / mkntfs are GPL-2.0-or-later; anything linking this inherits that.
 */

#include "mkntfs_glue.h"

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <locale.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "NTFSBridge.h"
#include "ntfsb_devio.h"

/* libntfs-3g param.h: largest cluster Windows 10 (1703+) accepts. */
#define NTFSB_MAX_CLUSTER_SIZE (2u * 1024u * 1024u)
/* mkntfs refuses volumes below 1 MiB. */
#define NTFSB_MIN_VOLUME_SIZE (1024u * 1024u)
/* Windows limits volume labels to 32 UTF-16 code units. */
#define NTFSB_MAX_LABEL_UNITS 32

static pthread_mutex_t g_format_lock = PTHREAD_MUTEX_INITIALIZER;

/* Unwind target for exit() calls inside mkntfs (see mkntfs_glue.h). Only
 * touched with g_format_lock held. */
static jmp_buf g_exit_jmp;
static bool g_exit_armed;

_Noreturn void ntfsb_mkntfs_exit(int status)
{
    (void)status; /* any exit from mkntfs means the format did not complete */
    if (!g_exit_armed)
        abort(); /* mkntfs code running outside ntfsb_format(): a bug */
    longjmp(g_exit_jmp, 1);
}

typedef struct format_progress {
    ntfsb_progress_fn fn;
    void *ctx;
    uint64_t expected_bytes;  /* bytes a full (zeroing) format writes, 0 for quick */
    double last_reported;
    bool cancelled;
} format_progress;

static bool report(format_progress *fp, double percent)
{
    if (!fp->fn || fp->cancelled)
        return !fp->cancelled;
    fp->last_reported = percent;
    if (fp->fn(fp->ctx, percent))
        fp->cancelled = true;
    return !fp->cancelled;
}

/*
 * Write observer. mkntfs has no progress hooks, but its IO pattern is
 * predictable: the first write ends initialisation; a full format then
 * zeroes the whole device front to back before writing the metadata. We
 * map bytes written to 5..95 % for full formats and report a single
 * "initialised" step for quick formats. Cancelling fails the next write,
 * which makes mkntfs abort cleanly through its error path.
 */
static int format_observer(void *ctx, uint64_t total_written)
{
    format_progress *fp = ctx;
    double percent;

    if (fp->expected_bytes == 0) {
        percent = 50.0;
    } else {
        double frac = (double)total_written / (double)fp->expected_bytes;
        if (frac > 1.0)
            frac = 1.0;
        percent = 5.0 + 90.0 * frac;
    }
    if (percent >= fp->last_reported + 1.0 || fp->last_reported == 0.0) {
        if (!report(fp, percent))
            return ECANCELED;
    }
    return 0;
}

/* Count UTF-16 code units of a UTF-8 string; -1 if it is not valid UTF-8. */
static long utf16_units(const char *s)
{
    static const uint32_t min_cp[4] = { 0, 0x80, 0x800, 0x10000 }; /* reject overlongs */
    const unsigned char *p = (const unsigned char *)s;
    long units = 0;
    while (*p) {
        unsigned c = *p;
        int extra;
        uint32_t cp;
        if (c < 0x80) { extra = 0; cp = c; }
        else if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
        else return -1;
        p++;
        for (int i = 0; i < extra; i++, p++) {
            if ((*p & 0xC0) != 0x80)
                return -1;
            cp = (cp << 6) | (*p & 0x3F);
        }
        if (cp < min_cp[extra] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return -1;
        units += cp >= 0x10000 ? 2 : 1;
    }
    return units;
}

/*
 * getopt keeps its scan position in globals; mkntfs runs getopt_long() from
 * scratch every time, so rewind it. glibc needs optind = 0 for a full
 * re-initialisation, BSD libc (macOS) uses optreset.
 */
static void reset_getopt(void)
{
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    optreset = 1;
    optind = 1;
#else
    optind = 0;
#endif
    opterr = 0;
}

/* Run mkntfs with exit() redirected here (kept apart so that no caller
 * locals live across setjmp). Returns mkntfs' exit status. */
static int run_mkntfs(int argc, char **argv)
{
    volatile int rc = 1;
    if (setjmp(g_exit_jmp) == 0) {
        g_exit_armed = true;
        rc = ntfsb_mkntfs_main(argc, argv);
    }
    g_exit_armed = false;
    return rc;
}

static int validate(const ntfsb_io *io, const ntfsb_format_opts *opts)
{
    if (!io || !opts || !io->pread || !io->pwrite || io->read_only)
        return io && io->read_only ? EROFS : EINVAL;
    if (io->block_size < 256 || io->block_size > 4096 ||
        (io->block_size & (io->block_size - 1)))
        return EINVAL; /* mkntfs supports 256..4096 byte sectors */
    if (io->size_bytes < NTFSB_MIN_VOLUME_SIZE)
        return ENOSPC;
    if (opts->cluster_size) {
        if ((opts->cluster_size & (opts->cluster_size - 1)) ||
            opts->cluster_size < io->block_size ||
            opts->cluster_size > NTFSB_MAX_CLUSTER_SIZE ||
            opts->cluster_size > 4096u * io->block_size)
            return EINVAL;
    }
    if (opts->hidden_sectors > UINT32_MAX)
        return EINVAL; /* boot sector field is 32 bits */
    if (opts->label_utf8 && opts->label_utf8[0]) {
        long units = utf16_units(opts->label_utf8);
        if (units < 0)
            return EILSEQ;
        if (units > NTFSB_MAX_LABEL_UNITS)
            return ENAMETOOLONG;
    }
    return 0;
}

int ntfsb_format(const ntfsb_io *io, const ntfsb_format_opts *opts,
                 ntfsb_progress_fn progress, void *progress_ctx)
{
    int err = validate(io, opts);
    if (err)
        return err;

    format_progress fp = {
        .fn = progress,
        .ctx = progress_ctx,
        .expected_bytes = opts->quick ? 0 : io->size_bytes,
    };
    if (!report(&fp, 0.0))
        return ECANCELED;

    /* All numeric arguments are formatted into this fixed storage. */
    char cluster[24], sector[24], hidden[24], sectors[24];
    snprintf(cluster, sizeof(cluster), "%" PRIu32, opts->cluster_size);
    snprintf(sector, sizeof(sector), "%" PRIu32, io->block_size);
    snprintf(hidden, sizeof(hidden), "%" PRIu64, opts->hidden_sectors);
    snprintf(sectors, sizeof(sectors), "%" PRIu64, io->size_bytes / io->block_size);

    /*
     * -F: our device reports S_IFBLK, but force anyway so mkntfs never
     *     refuses on account of mount-table heuristics.
     * -s/-p/-H/-S and the explicit sector count keep mkntfs from probing the
     *     geometry with ioctls. 255 heads x 63 sectors/track is the
     *     conventional LBA translation Windows itself writes; the values only
     *     matter for CHS booting.
     * The device name is never opened; the IO table ignores it.
     */
    char *argv[24];
    int argc = 0;
    argv[argc++] = "mkntfs";
    argv[argc++] = "-F";
    if (opts->quick)
        argv[argc++] = "-Q";
    if (opts->enable_compression)
        argv[argc++] = "-C";
    if (opts->label_utf8 && opts->label_utf8[0]) {
        argv[argc++] = "-L";
        argv[argc++] = (char *)opts->label_utf8;
    }
    if (opts->cluster_size) {
        argv[argc++] = "-c";
        argv[argc++] = cluster;
    }
    argv[argc++] = "-s";
    argv[argc++] = sector;
    argv[argc++] = "-p";
    argv[argc++] = hidden;
    argv[argc++] = "-H";
    argv[argc++] = "255";
    argv[argc++] = "-S";
    argv[argc++] = "63";
    argv[argc++] = "ntfsb-format-device";
    argv[argc++] = sectors;
    argv[argc] = NULL;

    pthread_mutex_lock(&g_format_lock);

    /* mkntfs calls setlocale(LC_ALL, ""); put the host's locale back after. */
    char *saved_locale = NULL;
    const char *cur = setlocale(LC_ALL, NULL);
    if (cur)
        saved_locale = strdup(cur);

    ntfsb_mkntfs_bind(io, format_observer, &fp);
    reset_getopt();
    int rc = run_mkntfs(argc, argv);
    ntfsb_mkntfs_bind(NULL, NULL, NULL);
    ntfsb_mkntfs_reset_state();
    reset_getopt();
    ntfsb_log_install();

    if (saved_locale) {
        setlocale(LC_ALL, saved_locale);
        free(saved_locale);
    }

    pthread_mutex_unlock(&g_format_lock);

    if (fp.cancelled)
        return ECANCELED;
    if (rc != 0)
        return EIO;
    /* mkntfs ends with d_ops->sync(), i.e. io->sync, before returning 0. */
    report(&fp, 100.0);
    return 0;
}
