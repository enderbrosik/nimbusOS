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
           -O2 -g -Wall -Wextra -Wno-unused-parameter -I. -DNIMBUS_KERNEL
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

.PHONY: all build run debug clean test
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

$(BUILD)/tools/%: tools/%.c
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOSTCFLAGS) $^ -o $@

$(IMG): $(MBR) $(STAGE2) $(KBIN) $(KELF) $(MKIMAGE)
	$(MKIMAGE) $@ $(IMG_MB) $(MBR) $(STAGE2) $(KBIN)

run: $(IMG)
	$(QEMU) $(QEMU_COMMON) -serial stdio $(QEMU_EXTRA)

debug: $(IMG)
	@echo "QEMU paused with GDB stub on localhost:1234. In another terminal run:"
	@echo "  gdb $(KELF) -ex 'target remote :1234' -ex 'break kmain' -ex continue"
	$(QEMU) $(QEMU_COMMON) -serial stdio -s -S $(QEMU_EXTRA)

clean:
	rm -rf $(BUILD)

-include $(DEPS)
