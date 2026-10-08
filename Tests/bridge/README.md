# Bridge self-test (Linux or macOS host)

`selftest.c` drives every function of `NTFSExtension/Bridge/NTFSBridge.h`
against a 64 MiB image file, once with a 512-byte and once with a 4096-byte
logical block size. Its `ntfsb_io` callbacks `abort()` on any request that is
not block aligned, so a pass also proves the bounce/RMW layer in
`ntfsb_devio.c`. Covered: format (incl. cancellation and re-running mkntfs
in-process), probe, mount flags (RW, RDONLY, read-only io, IGNORE_CASE,
SHOW_SYS_FILES, REMOVE_HIBER), unaligned read/write, truncate, setattr,
hard links, native + WSL symlinks, rename (move, replace, dir replace,
EINVAL/EISDIR/ENOTDIR/ENOTEMPTY, case-only), readdir resumption from every
cookie of a 300-entry directory (index root + index allocation), hibernation
detection, label, volume info, persistence across remounts, and finally
`ntfsfix -n` on the image when available.

## 1. Build libntfs-3g (Linux recipe used for the self-test)

```sh
SRC=/path/to/ntfs-3g_ntfsprogs-2022.10.3      # or a git checkout of tag 2022.10.3
# A git checkout has no ./configure; regenerate it. configure.ac references
# AM_PATH_LIBGCRYPT even with --disable-crypto, so give autoconf a stub:
mkdir -p /tmp/m4stub && echo 'AC_DEFUN([AM_PATH_LIBGCRYPT],[$3])' > /tmp/m4stub/gcrypt.m4
(cd "$SRC" && autoreconf -fi -I m4 -I /tmp/m4stub)

mkdir build && cd build
"$SRC"/configure --prefix="$PWD/../prefix" --disable-shared --enable-static \
    --disable-ntfs-3g --disable-plugins --disable-crypto --disable-ldconfig \
    --disable-mtab --without-uuid --without-hd --with-pic CFLAGS="-O2 -g"
make -j8
make -C libntfs-3g install && make -C include install   # (top-level `make install`
                                                       #  fails in an install hook; not needed)
```

Do **not** pass `--disable-device-default-io-ops`: mkntfs.c has an `#error`
for it, and the bridge's mkntfs rename relies on the default-IO branch of
`device_io.h`.

## 2. Build and run the test

```sh
cd Tests/bridge
make NTFS3G_SRC=$SRC NTFS3G_BUILD=$PWD/../../build NTFS3G_PREFIX=$PWD/../../prefix check
make ... SANITIZE=1 check     # ASan + UBSan (+ LeakSanitizer on Linux)
```

`NTFS3G_BUILD` is the configured build directory (for `config.h` and
`ntfsprogs/ntfsfix`); it defaults to `NTFS3G_SRC` for in-tree builds.

## 3. Compiling the bridge (Xcode target)

* Sources: `NTFSBridge.c`, `ntfsb_devio.c`, `mkntfs_glue.c` (C11/gnu11).
* Header search path: the directory that *contains* `ntfs-3g/` (the bridge
  includes `<ntfs-3g/volume.h>` etc.), i.e. `ThirdParty/ntfs-3g/include`.
  `-DHAVE_CONFIG_H` is optional: the bridge compiles with or without
  `config.h` on the path (both verified).
* `-DNTFSB_LIBNTFS_VERSION='"2022.10.3"'` (libntfs-3g has no runtime version
  getter; this default is compiled in if the define is missing).
* Link: `libmkntfs.a libntfs-3g.a` in that order, plus `-lpthread` on Linux.
  With `--with-uuid` (the macOS script) uuid_generate comes from libSystem.

## 4. libmkntfs.a — the known-good recipe (mirrored by scripts/build-libntfs3g.sh)

Sources (ntfsprogs/Makefile.am of 2022.10.3): `attrdef.c boot.c sd.c mkntfs.c utils.c`.

All five files:

```
-DHAVE_CONFIG_H -I<build dir with config.h> -I<src>/include/ntfs-3g -I<src>/ntfsprogs
-Dexit=ntfsb_mkntfs_exit
```

`mkntfs.c` additionally:

```
-DNTFSB_MKNTFS_UNIT -include <repo>/NTFSExtension/Bridge/mkntfs_glue.h
-Dmain=ntfsb_mkntfs_main            # optional; the glue header #undefs and replaces it
```

What the glue header does in unit mode (see its comments):

* `#define ntfs_device_unix_io_ops ntfsb_mkntfs_io_ops` — `device_io.h`
  itself does `#define ntfs_device_default_io_ops ntfs_device_unix_io_ops`,
  which would override a command-line `-Dntfs_device_default_io_ops=...`
  (ARCHITECTURE.md rule 6 as originally written does **not** work).
* Redefines `main` so that `ntfsb_mkntfs_reset_state()` is emitted inside
  mkntfs.c right before the renamed `ntfsb_mkntfs_main()`. mkntfs leaves
  `g_allocation` dangling and leaks `g_upcaseinfo` after each run; without
  the reset a second in-process format is a use-after-free.
* `#define exit ntfsb_mkntfs_exit` unless already defined on the command line.
  mkntfs 2022.10.3 never calls `exit()`; the bridge still defines
  `ntfsb_mkntfs_exit()` (longjmp back into `ntfsb_format`, result EIO).

Expected symbols (`nm libmkntfs.a`): `T ntfsb_mkntfs_main`,
`T ntfsb_mkntfs_reset_state`, `U ntfsb_mkntfs_io_ops`, and **no**
`ntfs_device_unix_io_ops` reference.

## Expected output (tail)

```
== block size 4096: build/scratch/ntfs-4096.img
...
NTFS partition build/scratch/ntfs-4096.img was processed successfully.
   io: 5306 reads, 6450 writes, 9 syncs (all block-aligned)
13064 checks, 0 failures
SELFTEST PASSED
```

mkntfs and libntfs-3g print a few expected messages on the way (mkntfs'
progress, "Failed to determine whether ntfsb-format-device is mounted" — the
dummy device name is never opened —, "This should not happen." /
"Operation canceled" from the deliberately cancelled format, and
"NTFS signature is missing." from probing a zeroed image).
