"""Packs the ARM9 and ARM7 binaries of the SWI test ROM into an .nds file.

    python -I mkrom.py [--twl] <arm9.bin> <arm7.bin> <out.nds>

Header layout from GBATEK "DS Cartridge Header". The ARM9 binary goes at
ROM offset 8000h (GBATEK: 4000h and up; melonDS's direct boot treats
4000h-7FFFh as the encrypted secure area, so the code starts after it) and
the ARM7 binary after it, both 1000h-aligned. There is no Nintendo logo;
the logo checksum field holds GBATEK's fixed value CF56h, and the header
checksum is the CRC-16 of 000h-15Dh (GBATEK "BIOS Misc Functions",
GetCRC16, seed FFFFh).

--twl writes a DSi-enhanced header (GBATEK "DSi Cartridge Header"): unit
code 02h, region flags FFFFFFFFh (region free), WRAMCNT 03h (all shared WRAM
to the ARM7, as in DS mode), flags 1BFh = 01h, title ID 00030000h:<gamecode
reversed>, and a 1000h-byte header. MBK1-9 stay 0 (no NWRAM mapped) and
there are no ARM9i/ARM7i areas (01Ch = 0). melonDS's DSi direct boot runs a
cart with unit code bit 1 set in DSi mode with the DSi BIOS active.
"""

import struct
import sys

ARM9_RAM = 0x02000000
ARM7_RAM = 0x02380000
ARM9_ROM = 0x8000


def crc16(data, crc=0xFFFF):
    vals = [0xC0C1, 0xC181, 0xC301, 0xC601, 0xCC01, 0xD801, 0xF001, 0xA001]
    for b in data:
        crc ^= b
        for j in range(8):
            carry = crc & 1
            crc >>= 1
            if carry:
                crc ^= vals[j] << (7 - j)
    return crc


def align(n, a):
    return (n + a - 1) & ~(a - 1)


def main():
    args = sys.argv[1:]
    twl = "--twl" in args
    if twl:
        args.remove("--twl")
    arm9 = open(args[0], "rb").read()
    arm7 = open(args[1], "rb").read()
    arm9 += b"\0" * (align(len(arm9), 4) - len(arm9))
    arm7 += b"\0" * (align(len(arm7), 4) - len(arm7))
    arm7_rom = align(ARM9_ROM + len(arm9), 0x1000)
    total = arm7_rom + len(arm7)
    capacity = 0
    while (0x20000 << capacity) < total:
        capacity += 1

    h = bytearray(0x1000 if twl else 0x200)
    h[0x000:0x00C] = b"SWI TESTDSI\0" if twl else b"SWI TESTROM\0"
    h[0x00C:0x010] = b"SWTD" if twl else b"SWTR"    # gamecode (not "####", so not a homebrew cart)
    h[0x010:0x012] = b"00"
    h[0x012] = 0x02 if twl else 0x00                # unit code: NDS+DSi / NDS
    h[0x014] = capacity
    struct.pack_into("<IIII", h, 0x020, ARM9_ROM, ARM9_RAM, ARM9_RAM, len(arm9))
    struct.pack_into("<IIII", h, 0x030, arm7_rom, ARM7_RAM, ARM7_RAM, len(arm7))
    struct.pack_into("<II", h, 0x060, 0x00586000, 0x001808F8)
    struct.pack_into("<H", h, 0x06E, 0x051E)
    struct.pack_into("<II", h, 0x080, total, 0x4000)
    if twl:
        h[0x1AF] = 0x03                                 # WRAMCNT
        struct.pack_into("<I", h, 0x1B0, 0xFFFFFFFF)    # region free
        h[0x1BF] = 0x01
        struct.pack_into("<4sI", h, 0x230, bytes(h[0x00C:0x010])[::-1], 0x00030000)
    struct.pack_into("<H", h, 0x15C, 0xCF56)
    struct.pack_into("<H", h, 0x15E, crc16(h[:0x15E]))

    rom = bytearray(h) + b"\0" * (ARM9_ROM - len(h))
    rom += arm9
    rom += b"\0" * (arm7_rom - len(rom))
    rom += arm7
    open(args[2], "wb").write(rom)
    print("%s: %d bytes (ARM9 %d at %Xh, ARM7 %d at %Xh), header CRC %04X"
          % (args[2], len(rom), len(arm9), ARM9_ROM, len(arm7), arm7_rom, crc16(h[:0x15E])))


if __name__ == "__main__":
    main()
