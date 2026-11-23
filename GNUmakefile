# Nuke built-in rules.
.SUFFIXES:

# Delete the target of a failed recipe.
.DELETE_ON_ERROR:

# Default target.
.DEFAULT_GOAL := all

# ---------------------------------------------------------------------------
# QEMU configuration
# ---------------------------------------------------------------------------

QEMUFLAGS := -m 2G
QEMU_MACHINE := -M q35
QEMU_CPU :=
QEMU_DEVICES :=

override QEMUFLAGS := \
    $(QEMU_MACHINE) \
    $(QEMU_CPU) \
    $(QEMU_DEVICES) \
    $(QEMUFLAGS)

# ---------------------------------------------------------------------------
# Project paths
# ---------------------------------------------------------------------------

BUILD_DIR := build
ISO_ROOT := $(BUILD_DIR)/iso_root

IMAGE_NAME := $(BUILD_DIR)/hua

LIMINE_DIR := third_party/limine/limine-binary
OVMF_DIR := third_party/ovmf/edk2-ovmf-bins

LIMINE_CONFIG := boot/limine/limine.conf
OVMF_CODE := $(OVMF_DIR)/ovmf-code-x86_64.fd

override QEMU_UEFI_FW := \
    -drive if=pflash,unit=0,format=raw,file=$(OVMF_CODE),readonly=on

# ---------------------------------------------------------------------------
# HDD image configuration
# ---------------------------------------------------------------------------

# User-controllable HDD image size in MiB.
HDD_SIZE := 64

# Internal HDD geometry. Do not change unless you understand the layout.
# 64 heads x 32 sectors makes each cylinder exactly 1 MiB.
override HDD_HEADS := 64
override HDD_SECTORS_PER_TRACK := 32
override HDD_CYLINDER_SECTORS := $(shell echo $$(( $(HDD_HEADS) * $(HDD_SECTORS_PER_TRACK) )))

# Leave the first and last cylinders for partition-table structures.
override HDD_PART_START := $(HDD_CYLINDER_SECTORS)
override HDD_PART_SECTORS := $(shell echo $$(( ($(HDD_SIZE) - 2) * $(HDD_CYLINDER_SECTORS) )))
override HDD_PART_END := $(shell echo $$(( $(HDD_PART_START) + $(HDD_PART_SECTORS) - 1 )))
override HDD_PART_OFFSET := $(shell echo $$(( $(HDD_PART_START) * 512 )))

# ---------------------------------------------------------------------------
# Host toolchain for the Limine utility
# ---------------------------------------------------------------------------

HOST_CC := cc
HOST_CFLAGS := -g -O2 -pipe
HOST_CPPFLAGS :=
HOST_LDFLAGS :=
HOST_LIBS :=

override VARSFILES := $(addprefix $(LIMINE_DIR)/,HOST_CC HOST_CFLAGS HOST_CPPFLAGS HOST_LDFLAGS HOST_LIBS)

override CHANGED_VARSFILES := $(foreach f,$(VARSFILES),$(shell printf '%s\n' '$(subst ','\'',$($(notdir $(f))))' | cmp -s - $(f) 2>/dev/null || echo $(f)))

.PHONY: FORCE
FORCE:

$(CHANGED_VARSFILES): FORCE

$(VARSFILES): $(LIMINE_DIR)/limine.c
	@printf '%s\n' '$(subst ','\'',$($(notdir $@)))' > $@

# Rebuild the host utility if its source or build configuration changes.
$(LIMINE_DIR)/limine: $(LIMINE_DIR)/limine.c $(VARSFILES)
	rm -f $@
	$(MAKE) -C $(LIMINE_DIR) \
		CC="$(HOST_CC)" \
		CFLAGS="$(HOST_CFLAGS)" \
		CPPFLAGS="$(HOST_CPPFLAGS)" \
		LDFLAGS="$(HOST_LDFLAGS)" \
		LIBS="$(HOST_LIBS)"

# ---------------------------------------------------------------------------
# Kernel
# ---------------------------------------------------------------------------

kernel/.deps-obtained:
	./kernel/get-deps

.PHONY: kernel
kernel: kernel/.deps-obtained
	$(MAKE) -C kernel

# ---------------------------------------------------------------------------
# Build directory
# ---------------------------------------------------------------------------

$(BUILD_DIR):
	mkdir -p "$@"

# ---------------------------------------------------------------------------
# Main targets
# ---------------------------------------------------------------------------

.PHONY: all
all: $(IMAGE_NAME).iso

.PHONY: all-hdd
all-hdd: $(IMAGE_NAME).hdd

# ---------------------------------------------------------------------------
# Run targets
# ---------------------------------------------------------------------------

.PHONY: run
run: $(IMAGE_NAME).iso
	qemu-system-x86_64 \
		-cdrom $(IMAGE_NAME).iso \
		-boot d \
		$(QEMUFLAGS)

.PHONY: run-uefi
run-uefi: $(OVMF_CODE) $(IMAGE_NAME).iso
	qemu-system-x86_64 \
		-cdrom $(IMAGE_NAME).iso \
		$(QEMUFLAGS) \
		$(QEMU_UEFI_FW)

.PHONY: run-hdd
run-hdd: $(IMAGE_NAME).hdd
	qemu-system-x86_64 \
		-hda $(IMAGE_NAME).hdd \
		$(QEMUFLAGS)

.PHONY: run-hdd-uefi
run-hdd-uefi: $(OVMF_CODE) $(IMAGE_NAME).hdd
	qemu-system-x86_64 \
		-hda $(IMAGE_NAME).hdd \
		$(QEMUFLAGS) \
		$(QEMU_UEFI_FW)

# ---------------------------------------------------------------------------
# ISO image
# ---------------------------------------------------------------------------

$(IMAGE_NAME).iso: $(LIMINE_DIR)/limine kernel | $(BUILD_DIR)
	rm -rf $(ISO_ROOT)
	mkdir -p $(ISO_ROOT)/boot
	cp kernel/bin/kernel $(ISO_ROOT)/boot/
	cp kernel/assets/terminus.psf $(ISO_ROOT)/boot/
	mkdir -p $(ISO_ROOT)/boot/limine
	cp $(LIMINE_CONFIG) \
		$(LIMINE_DIR)/limine-bios.sys \
		$(LIMINE_DIR)/limine-bios-cd.bin \
		$(LIMINE_DIR)/limine-uefi-cd.bin \
		$(ISO_ROOT)/boot/limine/
	mkdir -p $(ISO_ROOT)/EFI/BOOT
	cp $(LIMINE_DIR)/BOOTX64.EFI $(ISO_ROOT)/EFI/BOOT/
	cp $(LIMINE_DIR)/BOOTIA32.EFI $(ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
		-apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(ISO_ROOT) -o $(IMAGE_NAME).iso
	$(LIMINE_DIR)/limine bios-install $(IMAGE_NAME).iso
	rm -rf $(ISO_ROOT)

# ---------------------------------------------------------------------------
# HDD image
# ---------------------------------------------------------------------------

$(IMAGE_NAME).hdd: $(LIMINE_DIR)/limine kernel | $(BUILD_DIR)
	rm -f $(IMAGE_NAME).hdd
	dd if=/dev/null bs=1024k seek=$(HDD_SIZE) of=$(IMAGE_NAME).hdd
	PATH=$$PATH:/usr/sbin:/sbin sgdisk $(IMAGE_NAME).hdd \
		-n 1:$(HDD_PART_START):$(HDD_PART_END) -t 1:ef00 -m 1
	$(LIMINE_DIR)/limine bios-install $(IMAGE_NAME).hdd
	mformat -i $(IMAGE_NAME).hdd@@$(HDD_PART_OFFSET) \
		-T $(HDD_PART_SECTORS) \
		-h $(HDD_HEADS) \
		-s $(HDD_SECTORS_PER_TRACK) ::
	mmd -i $(IMAGE_NAME).hdd@@$(HDD_PART_OFFSET) \
		::/EFI ::/EFI/BOOT ::/boot ::/boot/limine
	mcopy -i $(IMAGE_NAME).hdd@@$(HDD_PART_OFFSET) \
		kernel/bin/kernel ::/boot
	mcopy -i $(IMAGE_NAME).hdd@@$(HDD_PART_OFFSET) \
		$(LIMINE_CONFIG) $(LIMINE_DIR)/limine-bios.sys ::/boot/limine
	mcopy -i $(IMAGE_NAME).hdd@@$(HDD_PART_OFFSET) \
		$(LIMINE_DIR)/BOOTX64.EFI ::/EFI/BOOT
	mcopy -i $(IMAGE_NAME).hdd@@$(HDD_PART_OFFSET) \
		$(LIMINE_DIR)/BOOTIA32.EFI ::/EFI/BOOT

# ---------------------------------------------------------------------------
# Cleanup
# ---------------------------------------------------------------------------

.PHONY: clean
clean:
	$(MAKE) -C kernel clean
	rm -rf $(ISO_ROOT) $(IMAGE_NAME).iso $(IMAGE_NAME).hdd

# Dependencies are kept in third_party/. Do not delete or reset them here.
# Preserve kernel/.deps-obtained so the next build does not rerun get-deps.
.PHONY: distclean
distclean: clean
	rm -rf kernel/.cache kernel/compile_commands.json
	rm -f third_party/*.tar.gz
