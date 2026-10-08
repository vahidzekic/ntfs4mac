#!/usr/bin/env bash
#
# build-libntfs3g.sh — fetch NTFS-3G and build, for macOS 15.4+:
#
#   ThirdParty/ntfs-3g/lib/libntfs-3g.a       static libntfs-3g, universal (arm64 + x86_64)
#   ThirdParty/ntfs-3g/lib/libmkntfs.a        mkntfs + helpers, renamed for in-process use
#   ThirdParty/ntfs-3g/include/config.h       the configure result the library was built with
#   ThirdParty/ntfs-3g/include/ntfs-3g/*.h    libntfs-3g public headers
#   ThirdParty/ntfs-3g/bin/{mkntfs,ntfsinfo,ntfsls,ntfscat,ntfsfix}
#                                             host-arch tools for making/inspecting test images
#   ThirdParty/ntfs-3g/COPYING, BUILDINFO     GPL text and the exact build recipe
#
# Usage:   scripts/build-libntfs3g.sh [--clean]
# Env:     NTFS3G_VERSION   (default 2022.10.3)
#          NTFS3G_SHA256    expected sha256 of the tarball (default: pinned value for 2022.10.3)
#          NTFS3G_URL       override the download URL
#          NTFS3G_ARCHS     (default "arm64 x86_64"; "arm64" for Apple-Silicon-only)
#          NTFS3G_TOOLS     1 (default) = also build host-arch ntfsprogs tools into bin/
#          NTFS3G_JOBS      parallel make jobs
#          NTFS3G_SKIP_VERIFY=1  accept an unverified tarball (prints its hash)
#
# Prerequisites (macOS host, Xcode 16.3+ or its Command Line Tools):
#   brew install autoconf automake libtool pkg-config gettext
#   The Tuxera release tarball ships a generated ./configure, so autotools are
#   only needed for the GitHub fallback (a bare git tag without ./configure);
#   that path also wants libgcrypt's m4 (brew install libgcrypt) or uses a stub.
#   pkg-config is needed because configure.ac calls PKG_PROG_PKG_CONFIG.
#
# Why not Homebrew's ntfs-3g? homebrew-core's `ntfs-3g` formula is Linux-only
# (`depends_on :linux`, "on macOS, requires closed-source macFUSE"), and the
# gromgit/fuse tap's `ntfs-3g-mac` requires macFUSE and builds the FUSE driver,
# not a static, FUSE-free, sandbox-friendly library pinned to our version and
# deployment target. So we build the library ourselves.
#
# mkntfs recipe: Tests/bridge/README.md and NTFSExtension/Bridge/mkntfs_glue.h
# (owned by the bridge engineer) are the source of truth for how libmkntfs.a
# is compiled and consumed. This script keeps the flags in one place, the
# MKNTFS_* variables below, and nowhere else.
#
set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration — every knob in one place.
# ---------------------------------------------------------------------------
NTFS3G_VERSION="${NTFS3G_VERSION:-2022.10.3}"

# sha256 of ntfs-3g_ntfsprogs-2022.10.3.tgz (1,324,320 bytes). Not computed in
# this repo's authoring environment (tuxera.com was unreachable there); taken
# from FreeBSD ports sysutils/fusefs-ntfs/distinfo and Arch Linux's PKGBUILD,
# which agree. The script prints the hash it computes, so check it once.
PINNED_SHA256_2022_10_3="f20e36ee68074b845e3629e6bced4706ad053804cbaf062fbae60738f854170c"

DEPLOYMENT_TARGET="15.4"
NTFS3G_ARCHS="${NTFS3G_ARCHS:-arm64 x86_64}"
NTFS3G_TOOLS="${NTFS3G_TOOLS:-1}"

# configure flags (all verified against configure.ac of 2022.10.3):
#   --disable-ntfs-3g     no FUSE driver; configure.ac then sets with_fuse=none, so
#                         neither libfuse nor macFUSE is looked for. (There is no
#                         separate --without-fuse switch worth passing: --with-fuse
#                         is only consulted when the FUSE driver is enabled.)
#   --disable-plugins     no dlopen()ed reparse-point plugins (they live in the FUSE driver anyway)
#   --disable-crypto      no gnutls/libgcrypt dependency (EFS decryption tools only)
#   --disable-nfconv      the macOS-only NFC<->NFD filename conversion patch needs
#                         CoreFoundation; we store names verbatim and let the
#                         bridge/FSKit layer decide on normalisation. Bridge
#                         engineer: flip to --enable-nfconv if you want it.
#   --disable-ldconfig/--disable-mtab  nothing to install system-wide, no /etc/mtab on macOS
#   --with-uuid           mkntfs generates a DCE volume GUID (uuid_generate is in libSystem)
#   --without-hd          no libhd geometry probing (Linux only)
CONFIGURE_FLAGS=(
    --disable-shared
    --enable-static
    --disable-ntfs-3g
    --enable-ntfsprogs
    --disable-plugins
    --disable-crypto
    --disable-nfconv
    --disable-ldconfig
    --disable-mtab
    --disable-posix-acls
    --disable-xattr-mappings
    --with-uuid
    --without-hd
)

# mkntfs sources: ntfsprogs/Makefile.am of 2022.10.3 says
#   mkntfs_SOURCES = attrdef.c attrdef.h boot.c boot.h sd.c sd.h mkntfs.c utils.c utils.h
MKNTFS_SOURCES=(attrdef.c boot.c sd.c mkntfs.c utils.c)

# Symbol renames that make mkntfs callable in-process (docs/ARCHITECTURE.md rule 6).
# The bridge owns the recipe in NTFSExtension/Bridge/mkntfs_glue.h. mkntfs.c is
# compiled with that header force-included in "unit" mode, and the header:
#   - renames main() to ntfsb_mkntfs_main() and puts ntfsb_mkntfs_reset_state()
#     into the same translation unit (mkntfs' globals are static);
#   - does `#define ntfs_device_unix_io_ops ntfsb_mkntfs_io_ops`.
#
# IMPORTANT deviation from ARCHITECTURE.md rule 6: include/ntfs-3g/device_io.h
# itself does `#define ntfs_device_default_io_ops ntfs_device_unix_io_ops`, so
# the command-line -Dntfs_device_default_io_ops=... from rule 6 is overridden by
# that header (macro redefinition, header wins) and mkntfs would do real POSIX
# file IO. Renaming the *target* of that macro works, because the preprocessor
# rescans the expansion: default -> unix -> ntfsb_mkntfs_io_ops. (Checked by
# compiling mkntfs.c from 2022.10.3 both ways and running nm.) The bridge
# defines `struct ntfs_device_operations ntfsb_mkntfs_io_ops`.
#
# If the glue header is missing (e.g. building libs before the bridge exists),
# the same renames are passed as -D flags. That gives ntfsb_mkntfs_main but no
# reset hook.
#
# -Dexit=...: mkntfs 2022.10.3 never calls exit() (it returns from main), so the
# objects reference no exit symbol at all. The rename is a guard: if a future
# NTFS-3G version adds an exit() call, the link fails on the undefined
# _ntfsb_mkntfs_exit instead of letting mkntfs terminate the extension.
MKNTFS_GLUE_HEADER="NTFSExtension/Bridge/mkntfs_glue.h"   # relative to the repo root
MKNTFS_COMMON_DEFINES=(-Dexit=ntfsb_mkntfs_exit)
MKNTFS_FALLBACK_DEFINES=(-Dmain=ntfsb_mkntfs_main -Dntfs_device_unix_io_ops=ntfsb_mkntfs_io_ops)

# ---------------------------------------------------------------------------
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/ThirdParty/ntfs-3g"
WORK="$ROOT/build/ntfs-3g"
TARBALL_NAME="ntfs-3g_ntfsprogs-${NTFS3G_VERSION}.tgz"
if [[ -f "$ROOT/$MKNTFS_GLUE_HEADER" ]]; then
    # Applied to mkntfs.c only; the helper files contain neither main() nor device IO.
    MKNTFS_UNIT_FLAGS=(-DNTFSB_MKNTFS_UNIT -include "$ROOT/$MKNTFS_GLUE_HEADER")
    MKNTFS_RECIPE="glue header $MKNTFS_GLUE_HEADER"
else
    MKNTFS_UNIT_FLAGS=("${MKNTFS_FALLBACK_DEFINES[@]}")
    MKNTFS_RECIPE="fallback -D renames (no $MKNTFS_GLUE_HEADER; no reset hook)"
fi
TARBALL="$WORK/$TARBALL_NAME"
SRC="$WORK/src/ntfs-3g_ntfsprogs-${NTFS3G_VERSION}"

log()  { printf '==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "'$1' not found. $2"; }

if [[ "${1:-}" == "--clean" ]]; then
    log "removing $WORK and $OUT"
    rm -rf "$WORK" "$OUT"
    shift
fi
[[ $# -eq 0 ]] || die "unknown argument: $1 (usage: $0 [--clean])"

[[ "$(uname -s)" == "Darwin" ]] || die "this script builds macOS binaries and must run on macOS (Linux bridge tests: see Tests/bridge/README.md)"
need xcrun   "Install Xcode or the Command Line Tools: xcode-select --install"
need clang   "Install Xcode or the Command Line Tools: xcode-select --install"
need lipo    "Install Xcode or the Command Line Tools: xcode-select --install"
need make    "Install Xcode or the Command Line Tools: xcode-select --install"
need curl    "curl ships with macOS"
need shasum  "shasum ships with macOS"
need pkg-config "brew install pkg-config"

SDK="$(xcrun --sdk macosx --show-sdk-path)"
JOBS="${NTFS3G_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
HOST_ARCH="$(uname -m)"
export MACOSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET"
# Homebrew's gettext puts a libintl.h on some include paths that #defines
# setlocale -> libintl_setlocale; libntfs-3g does not need libintl at all.
export ac_cv_header_libintl_h=no

mkdir -p "$WORK/src"

# ---------------------------------------------------------------------------
# 1. Fetch + verify
# ---------------------------------------------------------------------------
case "$NTFS3G_VERSION" in
    2022.10.3) DEFAULT_SHA256="$PINNED_SHA256_2022_10_3" ;;
    *)         DEFAULT_SHA256="" ;;
esac
EXPECTED_SHA256="${NTFS3G_SHA256:-$DEFAULT_SHA256}"

URLS=()
if [[ -n "${NTFS3G_URL:-}" ]]; then
    URLS+=("$NTFS3G_URL")
else
    URLS+=("https://tuxera.com/opensource/$TARBALL_NAME"
           "https://download.tuxera.com/opensource/$TARBALL_NAME")
fi
GITHUB_URL="https://github.com/tuxera/ntfs-3g/archive/refs/tags/${NTFS3G_VERSION}.tar.gz"

FROM_GITHUB=0
if [[ ! -s "$TARBALL" ]]; then
    fetched=0
    for url in "${URLS[@]}"; do
        log "fetching $url"
        if curl -fL --retry 3 --connect-timeout 20 -o "$TARBALL.part" "$url"; then
            mv "$TARBALL.part" "$TARBALL"; fetched=1; break
        fi
        warn "download failed: $url"
    done
    if [[ $fetched -eq 0 ]]; then
        log "release tarball unavailable; falling back to the GitHub tag (no ./configure; needs autotools)"
        curl -fL --retry 3 --connect-timeout 20 -o "$TARBALL.part" "$GITHUB_URL" \
            || die "could not download NTFS-3G $NTFS3G_VERSION from any source"
        mv "$TARBALL.part" "$TARBALL"
        FROM_GITHUB=1
        # The pinned hash is for the Tuxera tarball, not GitHub's archive.
        [[ -n "${NTFS3G_SHA256:-}" ]] || EXPECTED_SHA256=""
    fi
fi

ACTUAL_SHA256="$(shasum -a 256 "$TARBALL" | awk '{print $1}')"
log "sha256($TARBALL_NAME) = $ACTUAL_SHA256"
if [[ -n "$EXPECTED_SHA256" ]]; then
    if [[ "$ACTUAL_SHA256" != "$EXPECTED_SHA256" ]]; then
        rm -f "$TARBALL"
        die "sha256 mismatch: expected $EXPECTED_SHA256 (override with NTFS3G_SHA256=... if you trust the new file)"
    fi
    log "sha256 verified"
elif [[ "${NTFS3G_SKIP_VERIFY:-0}" == "1" ]]; then
    warn "no expected sha256 for this tarball; continuing because NTFS3G_SKIP_VERIFY=1"
else
    die "no expected sha256 for $TARBALL_NAME. Verify the hash printed above out of band, then re-run with NTFS3G_SHA256=$ACTUAL_SHA256 (or NTFS3G_SKIP_VERIFY=1)"
fi

# ---------------------------------------------------------------------------
# 2. Unpack (fresh every time, so stale objects never leak into a build)
# ---------------------------------------------------------------------------
rm -rf "$WORK/src" && mkdir -p "$WORK/src"
tar -xzf "$TARBALL" -C "$WORK/src"
if [[ ! -d "$SRC" ]]; then
    # GitHub archives unpack to ntfs-3g-<tag>/
    alt="$(find "$WORK/src" -mindepth 1 -maxdepth 1 -type d | head -n 1)"
    [[ -n "$alt" ]] || die "tarball did not unpack"
    mv "$alt" "$SRC"
fi
if [[ ! -x "$SRC/configure" ]]; then
    log "no ./configure (FROM_GITHUB=$FROM_GITHUB); running autoreconf"
    need autoreconf "brew install autoconf automake libtool"
    need automake   "brew install automake"
    if command -v glibtoolize >/dev/null 2>&1; then export LIBTOOLIZE=glibtoolize
    else need libtoolize "brew install libtool"; fi
    # configure.ac references AM_PATH_LIBGCRYPT even with --disable-crypto.
    # Without libgcrypt's m4 (brew install libgcrypt) autoconf aborts, so supply
    # a stub that always takes the "not found" branch.
    mkdir -p "$SRC/m4"
    if ! compgen -G "$(brew --prefix 2>/dev/null || echo /nonexistent)/share/aclocal/libgcrypt.m4" >/dev/null; then
        # shellcheck disable=SC2016 # $3 is an m4 argument, not a shell variable
        printf 'AC_DEFUN([AM_PATH_LIBGCRYPT],[$3])\n' >"$SRC/m4/zz-libgcrypt-stub.m4"
    fi
    (cd "$SRC" && autoreconf -fi -I m4)
fi
[[ -f "$SRC/COPYING" ]] || die "COPYING missing from source tree"

# ---------------------------------------------------------------------------
# 3. Per-architecture configure + build
# ---------------------------------------------------------------------------
BUILD_TRIPLET="$("$SRC/config.guess")"

build_arch() {
    local arch="$1" host bdir cflags
    case "$arch" in
        arm64)  host="aarch64-apple-darwin" ;;
        x86_64) host="x86_64-apple-darwin" ;;
        *) die "unsupported arch $arch" ;;
    esac
    local cache=()
    if [[ "$arch" == "$HOST_ARCH" ]]; then
        host="$BUILD_TRIPLET"          # native build: no cross-compilation mode
    else
        # Run-time checks that autoconf cannot execute when cross-compiling.
        cache=(ac_cv_func_memcmp_working=yes
               ac_cv_func_lstat_dereferences_slashed_symlink=yes
               ac_cv_func_stat_empty_string_bug=no
               ac_cv_func_malloc_0_nonnull=yes
               ac_cv_func_realloc_0_nonnull=yes)
    fi
    bdir="$WORK/build-$arch"
    rm -rf "$bdir" && mkdir -p "$bdir"
    cflags="-arch $arch -isysroot $SDK -mmacosx-version-min=$DEPLOYMENT_TARGET -O2 -g -fno-common"

    log "[$arch] configure (host=$host)"
    (
        cd "$bdir"
        env ${cache[@]+"${cache[@]}"} \
            CC="$(xcrun -f clang)" \
            CFLAGS="$cflags" \
            LDFLAGS="-arch $arch -isysroot $SDK -mmacosx-version-min=$DEPLOYMENT_TARGET" \
            "$SRC/configure" \
                --build="$BUILD_TRIPLET" --host="$host" \
                --prefix="$bdir/install" --exec-prefix="$bdir/install" \
                "${CONFIGURE_FLAGS[@]}" \
            >"$bdir/configure.log" 2>&1 \
            || { tail -n 40 "$bdir/configure.log" >&2; die "[$arch] configure failed (full log: $bdir/configure.log)"; }
    )

    log "[$arch] make libntfs-3g"
    make -C "$bdir/libntfs-3g" -j"$JOBS" >"$bdir/make-lib.log" 2>&1 \
        || { tail -n 60 "$bdir/make-lib.log" >&2; die "[$arch] libntfs-3g build failed (log: $bdir/make-lib.log)"; }
    [[ -f "$bdir/libntfs-3g/.libs/libntfs-3g.a" ]] || die "[$arch] libntfs-3g.a not produced"

    log "[$arch] compile libmkntfs.a (${MKNTFS_SOURCES[*]})"
    local objdir="$bdir/mkntfs-objs" src obj
    mkdir -p "$objdir"
    local unit_flags
    for src in "${MKNTFS_SOURCES[@]}"; do
        obj="$objdir/${src%.c}.o"
        unit_flags=()
        [[ "$src" == "mkntfs.c" ]] && unit_flags=("${MKNTFS_UNIT_FLAGS[@]}")
        # shellcheck disable=SC2086 # cflags is intentionally word-split
        "$(xcrun -f clang)" $cflags \
            -DHAVE_CONFIG_H \
            -I"$bdir" -I"$SRC/include/ntfs-3g" -I"$SRC/ntfsprogs" \
            ${unit_flags[@]+"${unit_flags[@]}"} \
            "${MKNTFS_COMMON_DEFINES[@]}" \
            -c "$SRC/ntfsprogs/$src" -o "$obj" 2>>"$bdir/make-mkntfs.log" \
            || { tail -n 40 "$bdir/make-mkntfs.log" >&2; die "[$arch] compiling $src failed"; }
    done
    rm -f "$bdir/libmkntfs.a"
    xcrun ar rcs "$bdir/libmkntfs.a" "$objdir"/*.o
    xcrun ranlib "$bdir/libmkntfs.a"

    if [[ "$NTFS3G_TOOLS" == "1" && "$arch" == "$HOST_ARCH" ]]; then
        log "[$arch] make host tools (mkntfs ntfsinfo ntfsls ntfscat ntfsfix)"
        make -C "$bdir/ntfsprogs" -j"$JOBS" mkntfs ntfsinfo ntfsls ntfscat ntfsfix \
            >"$bdir/make-tools.log" 2>&1 \
            || { tail -n 40 "$bdir/make-tools.log" >&2; warn "[$arch] tools build failed (non-fatal; log: $bdir/make-tools.log)"; }
    fi
}

read -r -a ARCH_LIST <<<"$NTFS3G_ARCHS"
[[ ${#ARCH_LIST[@]} -gt 0 ]] || die "NTFS3G_ARCHS is empty"
for arch in "${ARCH_LIST[@]}"; do
    build_arch "$arch"
done

# One config.h is shipped for all slices, so they must agree.
FIRST="${ARCH_LIST[0]}"
for arch in "${ARCH_LIST[@]}"; do
    [[ "$arch" != "$FIRST" ]] || continue
    if ! cmp -s "$WORK/build-$FIRST/config.h" "$WORK/build-$arch/config.h"; then
        diff -u "$WORK/build-$FIRST/config.h" "$WORK/build-$arch/config.h" >&2 || true
        if [[ "${NTFS3G_ALLOW_CONFIG_DIFF:-0}" == "1" ]]; then
            warn "config.h differs between $FIRST and $arch; continuing (NTFS3G_ALLOW_CONFIG_DIFF=1)"
        else
            die "config.h differs between $FIRST and $arch; refusing to ship one header for both (NTFS3G_ALLOW_CONFIG_DIFF=1 to override)"
        fi
    fi
done

# ---------------------------------------------------------------------------
# 4. Install into ThirdParty/ntfs-3g
# ---------------------------------------------------------------------------
log "installing into $OUT"
rm -rf "$OUT"
mkdir -p "$OUT/lib" "$OUT/include/ntfs-3g" "$OUT/bin"

lib_inputs=(); mk_inputs=()
for arch in "${ARCH_LIST[@]}"; do
    lib_inputs+=("$WORK/build-$arch/libntfs-3g/.libs/libntfs-3g.a")
    mk_inputs+=("$WORK/build-$arch/libmkntfs.a")
done
xcrun lipo -create "${lib_inputs[@]}" -output "$OUT/lib/libntfs-3g.a"
xcrun lipo -create "${mk_inputs[@]}"  -output "$OUT/lib/libmkntfs.a"
for arch in "${ARCH_LIST[@]}"; do
    xcrun lipo "$OUT/lib/libntfs-3g.a" -verify_arch "$arch"
    xcrun lipo "$OUT/lib/libmkntfs.a"  -verify_arch "$arch"
done

cp "$SRC"/include/ntfs-3g/*.h "$OUT/include/ntfs-3g/"
cp "$WORK/build-$FIRST/config.h" "$OUT/include/config.h"
cp "$SRC/COPYING" "$OUT/COPYING"
[[ -f "$SRC/COPYING.LIB" ]] && cp "$SRC/COPYING.LIB" "$OUT/COPYING.LIB"

if [[ "$NTFS3G_TOOLS" == "1" ]]; then
    tooldir="$WORK/build-$HOST_ARCH/ntfsprogs"
    for t in mkntfs ntfsinfo ntfsls ntfscat ntfsfix; do
        # libtool may leave the real binary in .libs/ when linking shared; we
        # build static only, so the top-level file is the binary.
        if [[ -x "$tooldir/$t" ]]; then cp "$tooldir/$t" "$OUT/bin/$t"; fi
    done
fi

# Sanity: the renamed entry points must be present, the originals absent.
nm_out="$(xcrun nm -g "$OUT/lib/libmkntfs.a" 2>/dev/null || true)"
grep -q ' T _ntfsb_mkntfs_main$' <<<"$nm_out" || die "libmkntfs.a does not define _ntfsb_mkntfs_main"
if [[ "$MKNTFS_RECIPE" == glue* ]]; then
    grep -q ' T _ntfsb_mkntfs_reset_state$' <<<"$nm_out" || die "libmkntfs.a does not define _ntfsb_mkntfs_reset_state (glue header not applied?)"
fi
grep -q ' U _ntfsb_mkntfs_io_ops$' <<<"$nm_out" || die "libmkntfs.a does not reference _ntfsb_mkntfs_io_ops (device IO rename failed)"
if grep -q ' U _ntfs_device_unix_io_ops$' <<<"$nm_out"; then
    die "libmkntfs.a still references _ntfs_device_unix_io_ops (mkntfs would write via POSIX IO)"
fi

{
    echo "NTFS-3G version : $NTFS3G_VERSION"
    echo "tarball sha256  : $ACTUAL_SHA256"
    echo "from GitHub tag : $FROM_GITHUB"
    echo "architectures   : ${ARCH_LIST[*]}"
    echo "deployment      : macOS $DEPLOYMENT_TARGET"
    echo "SDK             : $SDK"
    echo "configure flags : ${CONFIGURE_FLAGS[*]}"
    echo "mkntfs sources  : ${MKNTFS_SOURCES[*]}"
    echo "mkntfs recipe   : $MKNTFS_RECIPE"
    echo "mkntfs flags    : ${MKNTFS_UNIT_FLAGS[*]} ${MKNTFS_COMMON_DEFINES[*]}"
    echo "built           : $(date -u +%Y-%m-%dT%H:%M:%SZ)"
} >"$OUT/BUILDINFO"

log "done"
printf '    %s\n' \
    "$OUT/lib/libntfs-3g.a  ($(xcrun lipo -archs "$OUT/lib/libntfs-3g.a"))" \
    "$OUT/lib/libmkntfs.a   ($(xcrun lipo -archs "$OUT/lib/libmkntfs.a"))" \
    "$OUT/include/config.h, $OUT/include/ntfs-3g/*.h"
if compgen -G "$OUT/bin/*" >/dev/null; then
    printf '    %s\n' "$OUT/bin: $(cd "$OUT/bin" && echo *)"
fi
echo "    link order: -lmkntfs -lntfs-3g   (GPL-2.0-or-later — see $OUT/COPYING)"
