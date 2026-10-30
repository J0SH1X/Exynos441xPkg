#!/bin/bash
#
#  Build script for Exynos441xPkg (Samsung Galaxy Note 1 / Exynos4x1x).
#
#  Modelled on HtcLeoPkg's build.sh: sets up WORKSPACE/PACKAGES_PATH, builds
#  BaseTools once, applies the GenFw patches the ARM build needs, builds the
#  BootShim, runs `build`, then prepends the shim to the FD so the result is a
#  single raw image the Samsung bootloader will load from the KERNEL partition.
#
#  Usage:
#    ./build.sh              # DEBUG build, output on the framebuffer
#    ./build.sh --uart       # DEBUG build, output on UART0 (115200 8N1)
#    TARGET=RELEASE ./build.sh
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
#

set -e

export WORKSPACE=${WORKSPACE:-$PWD/workspace}
mkdir -p "$WORKSPACE"
export EDK_TOOLS_PATH=$PWD/../edk2/BaseTools
# NB: PACKAGES_PATH gets the parent of this package so that -p
# Exynos441xPkg/Exynos441xPkg.dsc resolves against it (same layout as
# HtcLeoPkg, whose package root is a PACKAGES_PATH entry).
export PACKAGES_PATH=$PWD/../edk2:$PWD/../edk2-platforms:$PWD/..

# Where the finished image lands
OUT_DIR=$PWD/Artifacts
mkdir -p "$OUT_DIR"

UART=0
for arg in "$@"; do
    case "${arg}" in
        --uart) UART=1 ;;
        *) echo "Unknown argument: ${arg} (only --uart is supported)"; exit 1 ;;
    esac
done

DSCOPTS=()
if [ "${UART}" = 1 ]; then
    DSCOPTS=(-D USE_SCREEN_FOR_SERIAL_OUTPUT=0 -D USE_UART_FOR_SERIAL_OUTPUT=1)
    echo "Debug output on UART0, framebuffer console off"
else
    DSCOPTS=(-D USE_SCREEN_FOR_SERIAL_OUTPUT=1 -D USE_UART_FOR_SERIAL_OUTPUT=0)
    echo "Debug output on the framebuffer (the phone has no UART)"
fi

function _patch_edk() {
    # GenFw cannot encode R_ARM_PC13 relocations (a clang-ism that shows up in
    # the ARM builds); map it onto R_ARM_REL32 like HtcLeoPkg does.
    local file="$EDK_TOOLS_PATH/Source/C/GenFw/Elf32Convert.c"
    if ! grep -q "case R_ARM_PC13:" "${file}"; then
        sed -i '/case R_ARM_REL32:/a\        case R_ARM_PC13:' "${file}"
        echo "Patched GenFw: R_ARM_PC13 -> R_ARM_REL32"
    fi

    # Some builds trip over an uninitialised Status in the English collation
    # driver; harmless to force to EFI_SUCCESS.
    local collate="$PWD/../edk2/MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/UnicodeCollationEng.c"
    if grep -q "EFI_STATUS  Status;" "${collate}"; then
        sed -i 's/EFI_STATUS  Status;/EFI_STATUS  Status = EFI_SUCCESS;/' "${collate}"
        echo "Patched UnicodeCollationEng.c"
    fi
}

function _base_tools() {
    if [ -n "${REBUILD_BASETOOLS}" ]; then
        make clean -C "$EDK_TOOLS_PATH"
        make -C "$EDK_TOOLS_PATH" -j"$(nproc)"
    elif [ ! -x "$EDK_TOOLS_PATH/Source/C/bin/VfrCompile" ] || \
         [ ! -x "$EDK_TOOLS_PATH/Source/C/bin/GenFw" ]; then
        make -C "$EDK_TOOLS_PATH" -j"$(nproc)"
    else
        echo "BaseTools present, skipping (REBUILD_BASETOOLS=1 to force)"
    fi
}

_patch_edk
_base_tools

# Build the BootShim: it carries the zImage magic the Samsung bootloader
# expects, cleans up CPU state, relocates the FD to UEFI_BASE and jumps to the
# GenFv reset vector at FD offset 0.
make -C BootShim UEFI_BASE=0x41000000 UEFI_SIZE=0x00100000

source "$PWD/../edk2/edksetup.sh"

# CLANGDWARF only defines DEPS_FLAGS for RISCV64.  Without it no ARM object
# depends on the headers it includes, so changing a shared struct rebuilt
# only some of a module's files and linked mismatched layouts (UsbFnDxe broke
# exactly that way).  Same flags GCC5/CLANGPDB use.
TOOLS_DEF="${CONF_PATH:-$PWD/../edk2/Conf}/tools_def.txt"
if ! grep -q '^\*_CLANGDWARF_ARM_DEPS_FLAGS' "$TOOLS_DEF"; then
    echo '*_CLANGDWARF_ARM_DEPS_FLAGS = -MMD -MF $@.deps' >> "$TOOLS_DEF"
    echo "Added CLANGDWARF ARM DEPS_FLAGS to $TOOLS_DEF"
fi

# shellcheck disable=SC2086
build -n "$(($(getconf _NPROCESSORS_ONLN) + 2))" \
      -a ARM -t CLANGDWARF \
      -p Exynos441xPkg/Exynos441xPkg.dsc \
      -b "${TARGET:-DEBUG}" \
      "${DSCOPTS[@]}"

FD="$WORKSPACE/Build/Exynos441x/${TARGET:-DEBUG}_CLANGDWARF/FV/EXYNOS441X_UEFI.fd"
if [ ! -f "$FD" ]; then
    echo "Build failed: $FD not found" >&2
    exit 1
fi

# bootpayload.bin = BootShim + FD, one contiguous image the bootloader loads
# as if it were a zImage.
PAYLOAD="$OUT_DIR/bootpayload.bin"
cat BootShim/BootShim.bin "$FD" > "$PAYLOAD"

# Downstream Linux booter: copy it next to a zImage on any FAT volume.
cp "$WORKSPACE/Build/Exynos441x/${TARGET:-DEBUG}_CLANGDWARF/ARM/LinuxBoot.efi" "$OUT_DIR/"

echo
echo "Build OK"
echo "  Firmware device : $FD ($(stat -c%s "$FD") bytes)"
echo "  Boot payload    : $PAYLOAD ($(stat -c%s "$PAYLOAD") bytes)"
echo
echo "Flash it as a raw zImage (Heimdall):"
echo "  heimdall flash --KERNEL $PAYLOAD"
echo
echo "Then boot the phone; you should see 'Exynos441xPkg UEFI' printed on"
echo "the screen followed by the DXE dispatch log and a UEFI Shell prompt."
