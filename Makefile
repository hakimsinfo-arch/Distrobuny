# Distro Buny — top-level build for the C components.
#
# Host tools (bunyctl, unit tests) are built dynamically for the build machine.
# Runtime binaries (buny-init, buny-ui, buny-preinit) are linked statically so
# they can run inside the runtime initramfs or a distribution's initramfs.
#
# Full image builds are driven by tools/build/buny-build, which calls this
# Makefile; see documentation/BUILD_INSTRUCTIONS.md.

BUILD    ?= build
CC       ?= gcc
VERSION  := $(shell cat VERSION 2>/dev/null || echo 0.0.0)
OPT      ?= -O2
CFLAGS   ?= $(OPT) -g
WARN     := -Wall -Wextra -Wno-unused-parameter -Wshadow -Wformat=2 -Wvla
DEFS     := -D_GNU_SOURCE -DBUNY_VERSION=\"$(VERSION)\"
CPPFLAGS += -Isource $(DEFS)
SEC      := -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fno-strict-aliasing
# Debug information records the build directory, and the linker's build ID is
# computed over it: map the directory away so the binaries do not depend on
# where the source tree lives (reproducible builds).
REPRO    := '-ffile-prefix-map=$(CURDIR)=.'
ALLCFLAGS = -std=gnu11 $(CFLAGS) $(WARN) $(SEC) $(REPRO) $(CPPFLAGS)

COMMON_SRC := source/common/util.c source/common/strbuf.c source/common/log.c \
              source/common/sha256.c source/common/json.c
FS_SRC     := source/filesystem/iso9660.c
CORE_SRC   := source/boot-adapters/grubcfg.c source/boot-adapters/bootcfg.c \
              source/boot-adapters/adapters.c source/boot-adapters/plan.c \
              source/iso-manager/identify.c source/iso-manager/catalog.c \
              source/iso-manager/rules.c
# Core library shared by the host tool, the runtime interface and unit tests.
LIB_SRC    := $(COMMON_SRC) $(FS_SRC) $(CORE_SRC)
# Runtime-only subsystems (block devices, kexec hand-off, Secure Boot state).
RT_SRC     := $(wildcard source/filesystem/blockdev.c source/filesystem/exfat.c source/filesystem/expand.c source/boot/cpio.c \
              source/boot/extents.c source/boot/handoff.c source/boot/ipc.c source/security/*.c)

UI_SRC     := $(wildcard source/interface/*.c)

HOST_OBJ_DIR    := $(BUILD)/host
RUNTIME_OBJ_DIR := $(BUILD)/runtime

host_obj    = $(patsubst source/%.c,$(HOST_OBJ_DIR)/%.o,$(1))
runtime_obj = $(patsubst source/%.c,$(RUNTIME_OBJ_DIR)/%.o,$(1))

.PHONY: all host runtime test clean
all: host runtime

host: $(BUILD)/bin/bunyctl $(BUILD)/bin/buny-ui-host

runtime: $(BUILD)/runtime-bin/buny-init $(BUILD)/runtime-bin/buny-ui $(BUILD)/runtime-bin/buny-preinit

$(HOST_OBJ_DIR)/%.o: source/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ALLCFLAGS) -MMD -MP -c $< -o $@

$(RUNTIME_OBJ_DIR)/%.o: source/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ALLCFLAGS) -DBUNY_RUNTIME=1 -MMD -MP -c $< -o $@

BUNYCTL_SRC := $(LIB_SRC) source/filesystem/blockdev.c source/filesystem/exfat.c source/filesystem/expand.c source/boot/cpio.c \
               source/boot/handoff.c source/boot/extents.c source/tools/bunyctl.c

$(BUILD)/bin/bunyctl: $(call host_obj,$(BUNYCTL_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ -lpthread -lm

# The interface built for the development machine (offscreen rendering, screenshots).
$(BUILD)/bin/buny-ui-host: $(call host_obj,$(LIB_SRC) $(RT_SRC) $(UI_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ -lpthread -lm

$(BUILD)/runtime-bin/buny-init: $(call runtime_obj,$(LIB_SRC) $(RT_SRC) source/boot/modload.c source/boot/init.c)
	@mkdir -p $(dir $@)
	$(CC) -static $(CFLAGS) -o $@ $^ -lpthread -lm
	strip $@

$(BUILD)/runtime-bin/buny-ui: $(call runtime_obj,$(LIB_SRC) $(RT_SRC) $(UI_SRC))
	@mkdir -p $(dir $@)
	$(CC) -static $(CFLAGS) -o $@ $^ -lpthread -lm
	strip $@

# The pre-init runs inside foreign initramfs images: keep it dependency free.
$(BUILD)/runtime-bin/buny-preinit: $(call runtime_obj,source/boot/preinit.c)
	@mkdir -p $(dir $@)
	$(CC) -static -Os -o $@ $^
	strip $@

# ------------------------------------------------------------------ tests
UNIT_TESTS := $(patsubst tests/unit/c/%.c,$(BUILD)/tests/%,$(wildcard tests/unit/c/test_*.c))

$(BUILD)/tests/%: tests/unit/c/%.c $(call host_obj,$(LIB_SRC) $(RT_SRC))
	@mkdir -p $(dir $@)
	$(CC) $(ALLCFLAGS) -Itests/unit/c -o $@ $^ -lpthread -lm

test: $(UNIT_TESTS)
	@set -e; for t in $(UNIT_TESTS); do echo "== $$t"; $$t; done

clean:
	rm -rf $(BUILD)

-include $(shell find $(HOST_OBJ_DIR) $(RUNTIME_OBJ_DIR) -type f -name '*.d' 2>/dev/null)
