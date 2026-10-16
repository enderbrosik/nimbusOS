# =============================================================================
# NimbusOS build
#
#   make build    - build the bootable disk image build/nimbus.img
#   make run      - boot it in QEMU
#   make debug    - boot it in QEMU paused, with a GDB stub on :1234
#   make test     - run the host-side file system test suite
#   make clean    - remove build output
#
# Toolchain: uses i686-elf-gcc if it is on PATH, otherwise clang targeting
# i686-elf with ld.lld.  Override with e.g. `make KCC=... KLD=...`.
# =============================================================================

BUILD   := build
IMG     := $(BUILD)/nimbus.img
IMG_MB  ?= 64

TOOLCHAIN ?= $(if $(shell command -v i686-elf-gcc 2>/dev/null),gcc,clang)
ifeq ($(TOOLCHAIN),gcc)
  KCC     ?= i686-elf-gcc
  KLD     ?= i686-elf-ld
  TARGET_FLAG :=
else
  KCC     ?= clang
  KLD     ?= $(if $(shell command -v ld.lld 2>/dev/null),ld.lld,ld)
  TARGET_FLAG := --target=i686-elf
endif
NASM    ?= nasm
HOSTCC  ?= cc
QEMU    ?= qemu-system-i386

CFLAGS  := $(TARGET_FLAG) -std=gnu11 -ffreestanding -fno-builtin -nostdlib -nostdinc \
           -fno-stack-protector -fno-pic -fno-omit-frame-pointer -fno-strict-aliasing \
           -mno-sse -mno-sse2 -mno-mmx -mno-80387 -m32 -march=i686 \
           -O2 -g -Wall -Wextra -Wno-unused-parameter -I. -DNIMBUS_KERNEL $(EXTRA_CFLAGS)
LDFLAGS := -m elf_i386 -T arch/x86/linker.ld -nostdlib
HOSTCFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -I.

KDIRS   := kernel arch/x86 drivers fs gui apps installer
KSRCS   := $(foreach d,$(KDIRS),$(wildcard $(d)/*.c))
KASMS   := arch/x86/entry.asm arch/x86/isr.asm
KOBJS   := $(patsubst %.asm,$(BUILD)/%.o,$(KASMS)) $(patsubst %.c,$(BUILD)/%.o,$(KSRCS))
DEPS    := $(KOBJS:.o=.d)

MBR     := $(BUILD)/boot/mbr.bin
STAGE2  := $(BUILD)/boot/stage2.bin
KELF    := $(BUILD)/kernel.elf
KBIN    := $(BUILD)/kernel.bin
MKIMAGE := $(BUILD)/tools/mkimage

QEMU_COMMON := -m 256M -vga std -drive file=$(IMG),format=raw,index=0,media=disk
QEMU_EXTRA  ?=

.PHONY: all build run debug clean test fsimage installer-test run-target test-all iso run-iso
all: build
build: $(IMG)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(KCC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/arch/x86/%.o: arch/x86/%.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf32 $< -o $@

$(BUILD)/boot/%.bin: arch/x86/boot/%.asm
	@mkdir -p $(dir $@)
	$(NASM) -f bin $< -o $@

$(KELF): $(KOBJS) arch/x86/linker.ld
	$(KLD) $(LDFLAGS) -o $@ $(KOBJS)

$(KBIN): $(KOBJS) arch/x86/linker.ld
	$(KLD) $(LDFLAGS) --oformat binary -o $@ $(KOBJS)

$(BUILD)/tools/mkimage: tools/mkimage.c
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOSTCFLAGS) $< -o $@

# Host tools that link the shared file system code.
$(BUILD)/tools/%: tools/%.c fs/nbfs.c fs/nbfs.h tools/hostdev.h
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOSTCFLAGS) $< fs/nbfs.c -o $@

# A standalone, empty 16 MiB NimbusFS image (demonstrates the formatter).
fsimage: $(NFSTOOL)
	$(NFSTOOL) mkfs $(BUILD)/empty-fs.img 16 EMPTY

test: $(BUILD)/tools/fstest
	@mkdir -p $(BUILD)/test
	$(BUILD)/tools/fstest

NFSTOOL := $(BUILD)/tools/nfstool
MKWALL  := $(BUILD)/tools/mkwallpaper
GENROOT := $(BUILD)/genroot
ROOTFS_FILES := $(shell find rootfs -type f 2>/dev/null)

$(MKWALL): tools/mkwallpaper.c
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOSTCFLAGS) $< -o $@ -lm

$(GENROOT)/.stamp: $(MKWALL)
	$(MKWALL) $(GENROOT)
	@touch $@

# The disk image: boot code + kernel, then a NimbusFS partition populated
# from rootfs/ (hand-written files) and $(GENROOT) (generated pictures).
$(IMG): $(MBR) $(STAGE2) $(KBIN) $(KELF) $(MKIMAGE) $(NFSTOOL) $(GENROOT)/.stamp $(ROOTFS_FILES)
	$(MKIMAGE) $@ $(IMG_MB) $(MBR) $(STAGE2) $(KBIN)
	$(NFSTOOL) format $@ NIMBUS
	$(NFSTOOL) import $@ rootfs /
	$(NFSTOOL) import $@ $(GENROOT) /

# ---- live CD --------------------------------------------------------------
# The ISO embeds a 32 MiB copy of the disk image flagged --ramdisk: the
# BIOS boots it via El Torito hard-disk emulation and stage 2 copies it into
# RAM, so the CD works as a live system (changes are lost at power off).
ISO      := $(BUILD)/nimbus.iso
CDIMG    := $(BUILD)/nimbus-cd.img
MKISO    := $(BUILD)/tools/mkiso

$(MKISO): tools/mkiso.c
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOSTCFLAGS) $< -o $@

$(CDIMG): $(MBR) $(STAGE2) $(KBIN) $(MKIMAGE) $(NFSTOOL) $(GENROOT)/.stamp $(ROOTFS_FILES)
	$(MKIMAGE) $@ 32 $(MBR) $(STAGE2) $(KBIN) --ramdisk
	$(NFSTOOL) format $@ NIMBUS
	$(NFSTOOL) import $@ rootfs /
	$(NFSTOOL) import $@ $(GENROOT) /

iso: $(ISO)
$(ISO): $(CDIMG) $(MKISO)
	$(MKISO) $@ $(CDIMG)

run-iso: $(ISO)
	$(QEMU) -m 256M -vga std -cdrom $(ISO) -boot d -serial stdio $(QEMU_EXTRA)

run: $(IMG)
	$(QEMU) $(QEMU_COMMON) -serial stdio $(QEMU_EXTRA)

debug: $(IMG)
	@echo "QEMU paused with GDB stub on localhost:1234. In another terminal run:"
	@echo "  gdb $(KELF) -ex 'target remote :1234' -ex 'break kmain' -ex continue"
	$(QEMU) $(QEMU_COMMON) -serial stdio -s -S $(QEMU_EXTRA)

# ---- installer testing ---------------------------------------------------
# A blank 64 MiB target disk image is created by `nfstool blank` (a sparse
# regular file - no real devices are involved), attached as the second
# drive (Primary Slave), and the live image boots from the first.
TARGET_IMG ?= $(BUILD)/target.img

$(TARGET_IMG): | $(NFSTOOL)
	$(NFSTOOL) blank $@ 64

installer-test: $(IMG) $(TARGET_IMG)
	@echo "Live image: $(IMG) (Primary Master), blank target: $(TARGET_IMG) (Primary Slave)"
	$(QEMU) -m 256M -vga std -drive file=$(IMG),format=raw,index=0,media=disk \
	        -drive file=$(TARGET_IMG),format=raw,index=1,media=disk -serial stdio $(QEMU_EXTRA)

# Boot only the installed target disk.
run-target:
	@test -f $(TARGET_IMG) || (echo "no $(TARGET_IMG) - run make installer-test first" && false)
	$(QEMU) -m 256M -vga std -drive file=$(TARGET_IMG),format=raw,index=0,media=disk -serial stdio $(QEMU_EXTRA)

# Run every automated test (host FS suite + all QEMU stage tests).
test-all: test $(IMG)
	python3 tools/test_stage1.py
	python3 tools/test_stage2.py
	python3 tools/test_stage3.py
	python3 tools/test_stage4.py
	python3 tools/test_stage5.py
	python3 tools/test_stage6.py
	python3 tools/test_stage7.py
	python3 tools/test_games.py
	python3 tools/test_iso.py

clean:
	rm -rf $(BUILD)

-include $(DEPS)
