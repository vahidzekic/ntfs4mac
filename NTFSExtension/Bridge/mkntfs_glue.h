/*
 * mkntfs_glue.h — run ntfsprogs' mkntfs in-process.
 *
 * mkntfs is a program, not a library: it has a main(), keeps its state in
 * file-scope globals and opens its device through libntfs-3g's default IO
 * table. This header serves two audiences:
 *
 * 1. The bridge (NTFSBridge.c / mkntfs_glue.c): declarations of the renamed
 *    mkntfs entry point and its state-reset hook.
 *
 * 2. The libmkntfs.a build. ntfsprogs/mkntfs.c is compiled with
 *
 *        -DNTFSB_MKNTFS_UNIT -include <path>/mkntfs_glue.h
 *
 *    which applies the macro renames below *before* mkntfs.c and the
 *    libntfs-3g headers are parsed. No source patch is needed. Extra
 *    -Dmain=ntfsb_mkntfs_main / -Dexit=ntfsb_mkntfs_exit on the command line
 *    are harmless: this header replaces `main` and defines `exit` the same
 *    way.
 *
 * libntfs-3g / mkntfs are GPL-2.0-or-later; anything linking this inherits that.
 */

#ifndef NTFSB_MKNTFS_GLUE_H
#define NTFSB_MKNTFS_GLUE_H

#ifdef NTFSB_MKNTFS_UNIT

/*
 * Device IO. mkntfs calls
 *     ntfs_device_alloc(name, 0, &ntfs_device_default_io_ops, NULL)
 * and libntfs-3g's device_io.h itself does
 *     #define ntfs_device_default_io_ops ntfs_device_unix_io_ops
 * which would silently override a -Dntfs_device_default_io_ops=... given on
 * the command line. Renaming the *target* of that macro instead survives the
 * header: the preprocessor rescans ntfs_device_unix_io_ops and lands on our
 * table. (mkntfs refuses to build with --disable-device-default-io-ops, so
 * the device_io.h branch above is always the one taken.)
 */
#define ntfs_device_unix_io_ops ntfsb_mkntfs_io_ops

/*
 * Entry point and re-entrancy. mkntfs_cleanup() frees the cluster
 * allocation list but leaves `g_allocation` dangling, never frees
 * `g_upcaseinfo`, and never resets the bad-block counter, so a second
 * in-process run would walk freed memory. Those globals are `static` in
 * mkntfs.c, so the reset function has to be defined inside that
 * translation unit. `main` appears exactly once in mkntfs.c — as the
 * definition at the very end, after every global — so we let the
 * object-like macro emit the reset function right in front of the renamed
 * main:
 *
 *     int main(int argc, char *argv[]) { ... }
 *  => int ntfsb_mkntfs_reset_state(void) { ... } int ntfsb_mkntfs_main(int argc, char *argv[]) { ... }
 *
 * A command-line -Dmain=ntfsb_mkntfs_main is replaced (not merely
 * redefined, which would warn) by this definition.
 */
#undef main
#define main                                                              \
    ntfsb_mkntfs_reset_state(void)                                        \
    {                                                                     \
        free(g_upcaseinfo);                                               \
        g_upcaseinfo = NULL;                                              \
        g_allocation = NULL; /* list already freed by mkntfs_cleanup() */ \
        g_num_bad_blocks = 0;                                             \
        g_mft_bitmap_byte_size = 0;                                       \
        g_lcn_bitmap_byte_size = 0;                                       \
        g_dynamic_buf_size = 0;                                           \
        g_mft_size = 0;                                                   \
        g_mft_lcn = 0;                                                    \
        g_mftmirr_lcn = 0;                                                \
        g_logfile_lcn = 0;                                                \
        g_logfile_size = 0;                                               \
        g_mft_zone_end = 0;                                               \
        return 0;                                                         \
    }                                                                     \
    int ntfsb_mkntfs_main

/*
 * mkntfs 2022.10.3 returns from main() on every error path, but an exit()
 * anywhere in the code compiled into the extension would kill the whole
 * file-system process. Route any such call to ntfsb_mkntfs_exit(), which
 * unwinds back into ntfsb_format(). Defined before the system headers are
 * read, so <stdlib.h> declares the renamed function (noreturn) for us.
 */
#ifndef exit
#define exit ntfsb_mkntfs_exit
#endif

/* Prototypes so the definitions above compile without warnings. */
int ntfsb_mkntfs_reset_state(void);
int ntfsb_mkntfs_main(int argc, char *argv[]);

#else /* !NTFSB_MKNTFS_UNIT */

/*
 * Replacement for exit() inside the mkntfs unit: longjmp()s back to the
 * running ntfsb_format(), which then fails with EIO (or ECANCELED). Must
 * only be reached while ntfsb_format() is executing mkntfs.
 */
_Noreturn void ntfsb_mkntfs_exit(int status);

/* mkntfs main(), renamed. argv[0] is the program name. Returns 0 on success. */
int ntfsb_mkntfs_main(int argc, char *argv[]);

/*
 * Return mkntfs' file-scope state to its initial values. Must be called
 * after every ntfsb_mkntfs_main() run (mkntfs_cleanup() already released
 * everything except what this resets).
 */
int ntfsb_mkntfs_reset_state(void);

/*
 * Re-install the bridge's libntfs-3g log handler. mkntfs' main() replaces
 * the process-global handler with its own (stdout/stderr, verbose);
 * defined in NTFSBridge.c.
 */
void ntfsb_log_install(void);

#endif /* NTFSB_MKNTFS_UNIT */

#endif /* NTFSB_MKNTFS_GLUE_H */
