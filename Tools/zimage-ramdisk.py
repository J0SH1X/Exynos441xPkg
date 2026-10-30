#!/usr/bin/env python3
"""Turn a dual-ramdisk zImage's appended ramdisk into an initrd for LinuxBoot.

Samsung/CF-Root-style kernels for the GT-N7000 (PhilZ & co.) append xz'd tar
archives after the zImage and record where in a "BOOT_IMAGE_OFFSETS" line
(boot_offset=..;boot_len=..;recovery_offset=..;recovery_len=..; in 512-byte
blocks).  At runtime their init reads them back from the KERNEL partition
(/dev/block/mmcblk0p5) - which holds UEFI when booting through LinuxBoot.efi,
so the extraction fails and e.g. PhilZ crash-loops on missing res/images.

This writes the chosen archive as an uncompressed newc cpio instead, which
LinuxBoot.efi -i passes as ATAG_INITRD2; the kernel unpacks it over its
built-in initramfs.

    zimage-ramdisk.py ZIMAGE OUT.cpio [recovery|boot]
"""
import io
import lzma
import re
import sys
import tarfile


def cpio_entry(name, mode, uid, gid, mtime, data=b"", nlink=1, ino=[0]):
    ino[0] += 1
    name = name.encode() + b"\0"
    hdr = b"070701" + b"".join(b"%08X" % v for v in (
        ino[0], mode, uid, gid, nlink, mtime, len(data), 0, 0, 0, 0, len(name), 0))
    out = hdr + name
    out += b"\0" * (-len(out) % 4)
    out += data
    out += b"\0" * (-len(out) % 4)
    return out


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    image, out = argv[0], argv[1]
    which = argv[2] if len(argv) > 2 else "recovery"

    d = open(image, "rb").read()
    m = re.search(rb"BOOT_IMAGE_OFFSETS[^ -~]*([ -~]+)", d)
    if not m:
        sys.exit("no BOOT_IMAGE_OFFSETS in " + image)
    fields = dict(kv.split("=") for kv in m.group(1).decode().strip(";").split(";"))
    off, blocks = int(fields[which + "_offset"]), int(fields[which + "_len"])
    blob = d[off * 512:(off + blocks) * 512]
    tar = tarfile.open(fileobj=io.BytesIO(lzma.LZMADecompressor().decompress(blob)))

    S_IFDIR, S_IFREG, S_IFLNK = 0o040000, 0o100000, 0o120000
    cpio = b""
    for t in tar.getmembers():
        name = t.name.lstrip("./")
        if not name:
            continue
        if t.isdir():
            cpio += cpio_entry(name, S_IFDIR | t.mode, t.uid, t.gid, t.mtime, nlink=2)
        elif t.issym():
            cpio += cpio_entry(name, S_IFLNK | 0o777, t.uid, t.gid, t.mtime, t.linkname.encode())
        elif t.isfile():
            cpio += cpio_entry(name, S_IFREG | t.mode, t.uid, t.gid, t.mtime,
                               tar.extractfile(t).read())
        else:
            print("skipping", t.name, "(type", t.type, ")")
    cpio += cpio_entry("TRAILER!!!", 0, 0, 0, 0)
    cpio += b"\0" * (-len(cpio) % 512)

    open(out, "wb").write(cpio)
    print(f"{which}: {len(tar.getmembers())} entries -> {out} ({len(cpio)} bytes)")


if __name__ == "__main__":
    main(sys.argv[1:])
