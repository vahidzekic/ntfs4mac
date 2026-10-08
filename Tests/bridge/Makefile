# Host-side self-test of the C bridge (Linux or macOS).
#
#   make NTFS3G_SRC=/path/to/ntfs-3g-source \
#        NTFS3G_BUILD=/path/to/configured-build-dir \
#        NTFS3G_PREFIX=/path/to/install-prefix  [SANITIZE=1] check
#
# NTFS3G_SRC     unpacked ntfs-3g_ntfsprogs-2022.10.3 tree (needs ntfsprogs/*.c)
# NTFS3G_BUILD   the directory ./configure ran in (config.h; optional ntfsprogs/ntfsfix)
#                defaults to NTFS3G_SRC (in-tree build)
# NTFS3G_PREFIX  `make install` prefix: lib/libntfs-3g.a, include/ntfs-3g/*.h
# See README.md for building those.

ifneq ($(MAKECMDGOALS),clean)
ifndef NTFS3G_SRC
$(error set NTFS3G_SRC to the ntfs-3g source tree)
endif
ifndef NTFS3G_PREFIX
$(error set NTFS3G_PREFIX to the libntfs-3g install prefix)
endif
endif
NTFS3G_BUILD  ?= $(NTFS3G_SRC)

ROOT    := $(abspath ../..)
BRIDGE  := $(ROOT)/NTFSExtension/Bridge
OUT     := build
CC      ?= cc

WARN    := -Wall -Wextra -Wno-unused-parameter
ifeq ($(SANITIZE),1)
SAN     := -fsanitize=address,undefined -fno-omit-frame-pointer
endif
CFLAGS  ?= -O1 -g
BASE    := $(CFLAGS) $(SAN) -std=gnu11 -pthread

# The bridge sees only the installed public headers, never config.h —
# exactly as the Xcode target does.
BRIDGE_CFLAGS  = $(BASE) $(WARN) -I$(BRIDGE) -I$(NTFS3G_PREFIX)/include \
                 -DNTFSB_LIBNTFS_VERSION='"2022.10.3"'

# mkntfs is built from the ntfsprogs sources with the libntfs-3g config.h,
# exactly like scripts/build-libntfs3g.sh does (see README.md).
MKNTFS_SRCS   := attrdef.c boot.c sd.c mkntfs.c utils.c
MKNTFS_CFLAGS  = $(BASE) -w -DHAVE_CONFIG_H -I$(NTFS3G_BUILD) \
                 -I$(NTFS3G_SRC)/include/ntfs-3g -I$(NTFS3G_SRC)/ntfsprogs \
                 -Dexit=ntfsb_mkntfs_exit
MKNTFS_UNIT   := -DNTFSB_MKNTFS_UNIT -include $(BRIDGE)/mkntfs_glue.h \
                 -Dmain=ntfsb_mkntfs_main

BRIDGE_OBJS := $(OUT)/NTFSBridge.o $(OUT)/ntfsb_devio.o $(OUT)/mkntfs_glue.o
MKNTFS_OBJS := $(MKNTFS_SRCS:%.c=$(OUT)/mkntfs/%.o)

.PHONY: all check clean
all: $(OUT)/selftest

$(OUT) $(OUT)/mkntfs:
	mkdir -p $@

$(OUT)/%.o: $(BRIDGE)/%.c $(BRIDGE)/NTFSBridge.h $(BRIDGE)/ntfsb_devio.h $(BRIDGE)/mkntfs_glue.h | $(OUT)
	$(CC) $(BRIDGE_CFLAGS) -c $< -o $@

$(OUT)/selftest.o: selftest.c $(BRIDGE)/NTFSBridge.h | $(OUT)
	$(CC) $(BASE) $(WARN) -I$(BRIDGE) -c $< -o $@

$(OUT)/mkntfs/mkntfs.o: $(NTFS3G_SRC)/ntfsprogs/mkntfs.c $(BRIDGE)/mkntfs_glue.h | $(OUT)/mkntfs
	$(CC) $(MKNTFS_CFLAGS) $(MKNTFS_UNIT) -c $< -o $@

$(OUT)/mkntfs/%.o: $(NTFS3G_SRC)/ntfsprogs/%.c | $(OUT)/mkntfs
	$(CC) $(MKNTFS_CFLAGS) -c $< -o $@

$(OUT)/libmkntfs.a: $(MKNTFS_OBJS)
	rm -f $@
	ar rcs $@ $^

$(OUT)/selftest: $(OUT)/selftest.o $(BRIDGE_OBJS) $(OUT)/libmkntfs.a $(NTFS3G_PREFIX)/lib/libntfs-3g.a
	$(CC) $(BASE) -o $@ $(OUT)/selftest.o $(BRIDGE_OBJS) $(OUT)/libmkntfs.a \
	    $(NTFS3G_PREFIX)/lib/libntfs-3g.a

NTFSFIX  = $(wildcard $(NTFS3G_BUILD)/ntfsprogs/ntfsfix)

check: $(OUT)/selftest
	rm -rf $(OUT)/scratch && mkdir -p $(OUT)/scratch
	./$(OUT)/selftest $(OUT)/scratch $(NTFSFIX)

clean:
	rm -rf $(OUT)
