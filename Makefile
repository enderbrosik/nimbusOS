# =============================================================================
# NimbusOS build
#
#   make build    - build the bootable disk image build/nimbus.img
#   make run      - boot it in QEMU
#   make debug    - boot it in QEMU paused, with a GDB stub on :1234
#   make test     - run the host-side file system test suite
#   make clean    - remove build output
#
# Toolchain: uses x86_64-elf-gcc if it is on PATH, otherwise clang targeting
# x86_64-elf with ld.lld.  Override with e.g. `make KCC=... KLD=...`.
# The kernel is 64-bit (long mode); the boot sectors are 16/32-bit.
# =============================================================================

BUILD   := build
IMG     := $(BUILD)/nimbus.img
IMG_MB  ?= 64

# Release version (shown at boot, compared by NimbusUpdate) and the default
# update server; `make release VERSION=0.3 UPDATE_URL=https://you.vercel.app`.
VERSION    ?= 0.2
UPDATE_URL ?= https://nimbusos.vercel.app
# The only host NimbusUpdate will contact (the test suite sets a local one).
UPDATE_HOST ?= nimbusos.vercel.app
# Update signing keys (public key compiled into the kernel, secret key signs releases).
UPDATE_PUBKEY ?= keys/update.pub
UPDATE_KEY    ?= keys/update.key

TOOLCHAIN ?= $(if $(shell command -v x86_64-elf-gcc 2>/dev/null),gcc,clang)
ifeq ($(TOOLCHAIN),gcc)
  KCC     ?= x86_64-elf-gcc
  KLD     ?= x86_64-elf-ld
  TARGET_FLAG :=
else
  KCC     ?= clang
  KLD     ?= $(if $(shell command -v ld.lld 2>/dev/null),ld.lld,ld)
  TARGET_FLAG := --target=x86_64-elf
endif
NASM    ?= nasm
HOSTCC  ?= cc
QEMU    ?= qemu-system-x86_64

CFLAGS  := $(TARGET_FLAG) -std=gnu11 -ffreestanding -fno-builtin -nostdlib -nostdinc \
           -fno-stack-protector -fno-pic -fno-omit-frame-pointer -fno-strict-aliasing \
           -mno-sse -mno-sse2 -mno-mmx -mno-80387 -m64 -mno-red-zone -mcmodel=small \
           -O2 -g -Wall -Wextra -Wno-unused-parameter -I. -I$(BUILD)/gen -DNIMBUS_KERNEL $(EXTRA_CFLAGS)
LDFLAGS := -m elf_x86_64 -z max-page-size=4096 -T arch/x86/linker.ld -nostdlib
HOSTCFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -I.

KDIRS   := kernel arch/x86 drivers fs gui apps installer net
KSRCS   := $(foreach d,$(KDIRS),$(wildcard $(d)/*.c))
KASMS   := arch/x86/entry.asm arch/x86/isr.asm
KOBJS   := $(patsubst %.asm,$(BUILD)/%.o,$(KASMS)) $(patsubst %.c,$(BUILD)/%.o,$(KSRCS))
DEPS    := $(KOBJS:.o=.d)

# ---- user programs (ring 3): libc + sample programs in user/ -------------
UCFLAGS := $(TARGET_FLAG) -std=gnu11 -ffreestanding -fno-builtin -nostdlib -nostdinc \
           -fno-stack-protector -fno-pic -mcmodel=large -mno-sse -mno-sse2 -mno-mmx -mno-80387 \
           -ffunction-sections -fdata-sections -O2 -g -Wall -Wextra -Iuser/include
ULIB_SRCS := $(wildcard user/lib/*.c)
ULIB_OBJS := $(BUILD)/user/lib/crt0.o $(patsubst %.c,$(BUILD)/%.o,$(ULIB_SRCS))
UPROGS    := $(patsubst user/programs/%.c,%,$(wildcard user/programs/*.c))
USERROOT  := $(BUILD)/userroot
UBINS     := $(patsubst %,$(USERROOT)/Programs/%.elf,$(UPROGS))
USRCS     := $(patsubst %,$(USERROOT)/Programs/Source/%.c,$(UPROGS))

$(BUILD)/user/%.o: user/%.c $(wildcard user/include/*.h)
	@mkdir -p $(dir $@)
	$(KCC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/lib/crt0.o: user/lib/crt0.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

$(USERROOT)/Programs/%.elf: $(BUILD)/user/programs/%.o $(ULIB_OBJS) user/linker.ld
	@mkdir -p $(dir $@)
	$(KLD) -m elf_x86_64 -z max-page-size=4096 --gc-sections --strip-debug -T user/linker.ld -nostdlib -o $@ $< $(ULIB_OBJS)

$(USERROOT)/Programs/Source/%.c: user/programs/%.c
	@mkdir -p $(dir $@)
	cp $< $@

programs: $(UBINS) $(USRCS)

MBR     := $(BUILD)/boot/mbr.bin
STAGE2  := $(BUILD)/boot/stage2.bin
KELF    := $(BUILD)/kernel.elf
KBIN    := $(BUILD)/kernel.bin
MKIMAGE := $(BUILD)/tools/mkimage

# Sound: AC'97 card. Host backend: CoreAudio on macOS, silent elsewhere
# (pick one with e.g. `make run AUDIO=pa` / `AUDIO=alsa` / `AUDIO=sdl`).
AUDIO   ?= $(if $(filter Darwin,$(shell uname -s)),coreaudio,none)
QEMU_SND := -audiodev $(AUDIO),id=snd0 -device AC97,audiodev=snd0 -nic user,model=e1000
QEMU_VGA := -device VGA,vgamem_mb=64
QEMU_COMMON := -m 256M $(QEMU_VGA) $(QEMU_SND) -drive file=$(IMG),format=raw,index=0,media=disk
QEMU_EXTRA  ?=

.PHONY: all build run debug clean test fsimage installer-test run-target test-all iso run-iso programs release keys deploy FORCE
all: build
build: $(IMG)

# Generated headers: the version / default update URL (rewritten only when
# they change, so dependent objects rebuild) and the update signing key.
GEN := $(BUILD)/gen
$(GEN)/nimbus_version.h: FORCE
	@mkdir -p $(GEN)
	@printf '#define NIMBUS_VERSION "%s"\n#define NIMBUS_UPDATE_URL "%s"\n#define NIMBUS_UPDATE_HOST "%s"\n' '$(VERSION)' '$(UPDATE_URL)' '$(UPDATE_HOST)' > $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv $@.tmp $@
$(GEN)/update_pubkey.h: $(wildcard $(UPDATE_PUBKEY))
	@mkdir -p $(GEN)
	@printf '#define NIMBUS_UPDATE_PUBKEY "%s"\n' "$$(cat $(UPDATE_PUBKEY) 2>/dev/null | tr -d ' \n')" > $@
FORCE:

# The UI font: DejaVu outlines rasterised by a host tool (the kernel has no FPU).
$(GEN)/ui_font.c: $(BUILD)/tools/mkfont fonts/DejaVuSans.ttf fonts/DejaVuSans-Bold.ttf
	@mkdir -p $(GEN)
	$(BUILD)/tools/mkfont $@ fonts/DejaVuSans.ttf fonts/DejaVuSans-Bold.ttf
$(BUILD)/gen/ui_font.o: $(GEN)/ui_font.c gui/fontdata.h
	@mkdir -p $(dir $@)
	$(KCC) $(CFLAGS) -c $< -o $@
KOBJS += $(BUILD)/gen/ui_font.o

$(BUILD)/%.o: %.c | $(GEN)/nimbus_version.h $(GEN)/update_pubkey.h
	@mkdir -p $(dir $@)
	$(KCC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/arch/x86/%.o: arch/x86/%.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

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

# Crypto test vectors and a TLS client test tool (both link net/ code on the host).
$(BUILD)/tools/cryptotest: tools/cryptotest.c net/crypto.c net/crypto.h
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOSTCFLAGS) tools/cryptotest.c net/crypto.c -o $@

$(BUILD)/tools/tlstest: tools/tlstest.c net/tls.c net/tls.h net/crypto.c net/crypto.h
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOSTCFLAGS) tools/tlstest.c net/tls.c net/crypto.c -o $@

# ---- NimbusUpdate releases --------------------------------------------------
# `make keys` once (creates keys/update.key - keep it secret - and keys/update.pub,
# which every kernel built afterwards trusts), then for each release:
#   make release VERSION=0.3 UPDATE_URL=https://<project>.vercel.app
#   cd build/update && vercel deploy --prod
RELEASE_OUT ?= build/update
RELEASE_NOTES ?= release-notes.txt
REL_BUILD   := build/release-$(VERSION)

$(BUILD)/tools/nimbuskey: tools/nimbuskey.c net/crypto.c net/crypto.h
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOSTCFLAGS) tools/nimbuskey.c net/crypto.c -o $@

$(BUILD)/tools/mkrelease: tools/mkrelease.c net/crypto.c net/crypto.h
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOSTCFLAGS) tools/mkrelease.c net/crypto.c -o $@

keys: $(BUILD)/tools/nimbuskey
	$(BUILD)/tools/nimbuskey keygen keys

release: $(BUILD)/tools/mkrelease
	@test -f $(UPDATE_KEY) || (echo "no $(UPDATE_KEY) - run 'make keys' first" && false)
	$(MAKE) BUILD=$(REL_BUILD) VERSION=$(VERSION) UPDATE_URL=$(UPDATE_URL) UPDATE_HOST=$(UPDATE_HOST) UPDATE_PUBKEY=$(UPDATE_PUBKEY) build
	@mkdir -p $(RELEASE_OUT)
	find $(RELEASE_OUT) -mindepth 1 -maxdepth 1 ! -name .vercel -exec rm -rf {} +   # keep the Vercel project link
	$(BUILD)/tools/mkrelease $(RELEASE_OUT) $(VERSION) $(REL_BUILD)/kernel.bin $(UPDATE_KEY) $(RELEASE_NOTES) \
	    rootfs $(REL_BUILD)/genroot $(REL_BUILD)/userroot

# Build, sign and publish a release with the Vercel CLI (npm i -g vercel).
# The first run asks you to log in and to create/link a project; the link is
# kept in build/update/.vercel so later deploys go to the same project.
deploy: release
	@command -v vercel >/dev/null || (echo "Vercel CLI not found - install it with: npm i -g vercel" && false)
	cd $(RELEASE_OUT) && vercel deploy --prod
	@echo "Done. On NimbusOS, point the updater at your project's URL once: update url https://<project>.vercel.app"

# Calculator arithmetic (apps/calc_math.h) on the host.
$(BUILD)/tools/calcmath: tools/calcmath.c apps/calc_math.h
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOSTCFLAGS) tools/calcmath.c -o $@

test: $(BUILD)/tools/fstest $(BUILD)/tools/cryptotest $(BUILD)/tools/calcmath
	@mkdir -p $(BUILD)/test
	$(BUILD)/tools/fstest
	$(BUILD)/tools/cryptotest
	$(BUILD)/tools/calcmath

$(BUILD)/tools/mkfont: tools/mkfont.c third_party/stb/stb_truetype.h
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOSTCFLAGS) -Wno-unused-function -Wno-sign-compare $< -o $@ -lm

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
$(IMG): $(MBR) $(STAGE2) $(KBIN) $(KELF) $(MKIMAGE) $(NFSTOOL) $(GENROOT)/.stamp $(ROOTFS_FILES) $(UBINS) $(USRCS)
	$(MKIMAGE) $@ $(IMG_MB) $(MBR) $(STAGE2) $(KBIN)
	$(NFSTOOL) format $@ NIMBUS
	$(NFSTOOL) import $@ rootfs /
	$(NFSTOOL) import $@ $(GENROOT) /
	$(NFSTOOL) import $@ $(USERROOT) /

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

$(CDIMG): $(MBR) $(STAGE2) $(KBIN) $(MKIMAGE) $(NFSTOOL) $(GENROOT)/.stamp $(ROOTFS_FILES) $(UBINS) $(USRCS)
	$(MKIMAGE) $@ 32 $(MBR) $(STAGE2) $(KBIN) --ramdisk
	$(NFSTOOL) format $@ NIMBUS
	$(NFSTOOL) import $@ rootfs /
	$(NFSTOOL) import $@ $(GENROOT) /
	$(NFSTOOL) import $@ $(USERROOT) /

iso: $(ISO)
$(ISO): $(CDIMG) $(MKISO)
	$(MKISO) $@ $(CDIMG)

run-iso: $(ISO)
	$(QEMU) -m 256M $(QEMU_VGA) $(QEMU_SND) -cdrom $(ISO) -boot d -serial stdio $(QEMU_EXTRA)

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
	$(QEMU) -m 256M $(QEMU_VGA) $(QEMU_SND) -drive file=$(IMG),format=raw,index=0,media=disk \
	        -drive file=$(TARGET_IMG),format=raw,index=1,media=disk -serial stdio $(QEMU_EXTRA)

# Boot only the installed target disk.
run-target:
	@test -f $(TARGET_IMG) || (echo "no $(TARGET_IMG) - run make installer-test first" && false)
	$(QEMU) -m 256M $(QEMU_VGA) $(QEMU_SND) -drive file=$(TARGET_IMG),format=raw,index=0,media=disk -serial stdio $(QEMU_EXTRA)

# Run every automated test (host FS suite + all QEMU stage tests).
test-all: test $(IMG)
	python3 tools/test_stage1.py
	python3 tools/test_stage2.py
	python3 tools/test_stage3.py
	python3 tools/test_stage4.py
	python3 tools/test_stage5.py
	python3 tools/test_stage6.py
	python3 tools/test_paint.py
	python3 tools/test_clipboard.py
	python3 tools/test_stage7.py
	python3 tools/test_games.py
	python3 tools/test_breakout.py
	python3 tools/test_polish.py
	python3 tools/test_sound.py
	python3 tools/test_iso.py
	python3 tools/test_net.py
	python3 tools/test_user.py
	python3 tools/test_https.py
	python3 tools/test_css.py
	python3 tools/test_search.py
	python3 tools/test_update.py

clean:
	rm -rf $(BUILD)

-include $(DEPS)
