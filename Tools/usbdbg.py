#!/usr/bin/env python3
"""Talk to the UsbLogDxe debug channel (1209:0001, interface 1).

    usbdbg.py ping
    usbdbg.py peek ADDR [COUNT]          32-bit words, hex dump
    usbdbg.py dump ADDR LEN [FILE]       raw bytes (to FILE or hex to stdout)
    usbdbg.py poke ADDR VALUE            32-bit write
    usbdbg.py reboot [normal|download|recovery]
    usbdbg.py flash [PAYLOAD]            reboot to download mode + heimdall flash

Numbers accept 0x prefixes.  Works next to Tools/usblog.py (interface 0).
Uses libusb-1.0 through ctypes; run as root or add a udev rule for 1209:0001.
"""
import ctypes
import os
import struct
import subprocess
import sys
import time

VID, PID = 0x1209, 0x0001
DL_VID, DL_PID = 0x04E8, 0x685D              # Samsung download mode
EP_OUT, EP_IN, IFACE = 0x02, 0x82, 1
REQ_MAGIC, RSP_MAGIC = 0x47424455, 0x52424455  # 'UDBG', 'UDBR'
PING, READ, WRITE32, RESET = 1, 2, 3, 4
MAX_READ = 4096
MODES = {"normal": 0, "download": 1, "recovery": 4}
DEFAULT_PAYLOAD = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "..", "Artifacts", "bootpayload.bin")

lib = ctypes.CDLL("libusb-1.0.so.0")
lib.libusb_open_device_with_vid_pid.restype = ctypes.c_void_p
lib.libusb_open_device_with_vid_pid.argtypes = [ctypes.c_void_p, ctypes.c_uint16, ctypes.c_uint16]
lib.libusb_bulk_transfer.argtypes = [ctypes.c_void_p, ctypes.c_ubyte, ctypes.c_char_p,
                                     ctypes.c_int, ctypes.POINTER(ctypes.c_int), ctypes.c_uint]
lib.libusb_claim_interface.argtypes = [ctypes.c_void_p, ctypes.c_int]
lib.libusb_release_interface.argtypes = [ctypes.c_void_p, ctypes.c_int]
lib.libusb_close.argtypes = [ctypes.c_void_p]
lib.libusb_error_name.restype = ctypes.c_char_p
lib.libusb_init(None)


def num(s):
    return int(s, 0)


def present(vid, pid):
    h = lib.libusb_open_device_with_vid_pid(None, vid, pid)
    if h:
        lib.libusb_close(h)
    return bool(h)


class Dbg:
    def __init__(self):
        self.h = lib.libusb_open_device_with_vid_pid(None, VID, PID)
        if not self.h:
            sys.exit("1209:0001 not found - phone not in UEFI, or not root?")
        rc = lib.libusb_claim_interface(self.h, IFACE)
        if rc:
            sys.exit(f"claim interface {IFACE}: {lib.libusb_error_name(rc).decode()}")

    def close(self):
        lib.libusb_release_interface(self.h, IFACE)
        lib.libusb_close(self.h)

    def call(self, cmd, a0=0, a1=0, expect=0):
        req = struct.pack("<IIII", REQ_MAGIC, cmd, a0, a1)
        done = ctypes.c_int(0)
        rc = lib.libusb_bulk_transfer(self.h, EP_OUT, req, len(req), ctypes.byref(done), 2000)
        if rc:
            sys.exit(f"send: {lib.libusb_error_name(rc).decode()}")
        # The reply is one transfer: header + data.  Ask for exactly that much
        # so a 512-multiple reply without a short packet still completes.
        buf = ctypes.create_string_buffer(16 + expect)
        rc = lib.libusb_bulk_transfer(self.h, EP_IN, buf, 16 + expect, ctypes.byref(done), 3000)
        if rc:
            sys.exit(f"receive: {lib.libusb_error_name(rc).decode()}")
        magic, status, length, _ = struct.unpack_from("<IIII", buf.raw)
        if magic != RSP_MAGIC:
            sys.exit(f"bad reply magic {magic:#x}")
        if status:
            sys.exit(f"device error {status}")
        return buf.raw[16:16 + length]

    def read(self, addr, length):
        out = b""
        while length:
            n = min(length, MAX_READ)
            out += self.call(READ, addr, n, expect=n)
            addr += n
            length -= n
        return out


def hexdump(addr, data, words):
    for off in range(0, len(data), 16):
        chunk = data[off:off + 16]
        if words:
            cols = " ".join(f"{w:08x}" for w in struct.unpack_from(f"<{len(chunk) // 4}I", chunk))
        else:
            cols = " ".join(f"{b:02x}" for b in chunk)
        print(f"{addr + off:08x}: {cols}")


def reboot(mode):
    d = Dbg()
    d.call(RESET, MODES[mode])
    d.close()
    print(f"rebooting ({mode})")


def flash(payload):
    if not os.path.exists(payload):
        sys.exit(f"{payload} not found")
    if not present(DL_VID, DL_PID):
        reboot("download")
        for _ in range(60):
            if present(DL_VID, DL_PID):
                break
            time.sleep(0.5)
        else:
            sys.exit("download mode device 04e8:685d did not show up")
        time.sleep(1)
    print(f"flashing {payload}")
    subprocess.run(["heimdall", "flash", "--KERNEL", payload], check=True)


def main(argv):
    if not argv:
        sys.exit(__doc__)
    cmd, args = argv[0], argv[1:]

    if cmd == "flash":
        flash(args[0] if args else DEFAULT_PAYLOAD)
        return
    if cmd == "reboot":
        reboot(args[0] if args else "normal")
        return

    d = Dbg()
    if cmd == "ping":
        print(d.call(PING, expect=4).decode())
    elif cmd == "peek":
        addr = num(args[0]) & ~3
        count = num(args[1]) if len(args) > 1 else 1
        hexdump(addr, d.read(addr, count * 4), words=True)
    elif cmd == "dump":
        addr, length = num(args[0]), num(args[1])
        data = d.read(addr, length)
        if len(args) > 2:
            open(args[2], "wb").write(data)
            print(f"{len(data)} bytes -> {args[2]}")
        else:
            hexdump(addr, data, words=False)
    elif cmd == "poke":
        d.call(WRITE32, num(args[0]), num(args[1]))
    else:
        sys.exit(__doc__)
    d.close()


if __name__ == "__main__":
    main(sys.argv[1:])
