#!/usr/bin/env python3
# Post-link patch for cosmo APE binaries: bumps the embedded PE header's
# SizeOfStackReserve / SizeOfStackCommit fields. Cosmocc's default of
# 64 KiB / 4 KiB is too small for Windows.
#
# Usage: patch_pe_stack.py <ape_binary> <reserve_bytes> <commit_bytes>

import struct
import sys


def main(path: str, reserve: int, commit: int) -> int:
    data = bytearray(open(path, "rb").read())
    pe = data.find(b"PE\x00\x00")
    if pe < 0:
        print(f"patch_pe_stack: no PE signature in {path}", file=sys.stderr)
        return 1
    opt = pe + 4 + 20  # skip PE sig + COFF header
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic != 0x20B:
        print(f"patch_pe_stack: expected PE32+ (0x20b), got 0x{magic:x}",
              file=sys.stderr)
        return 1
    struct.pack_into("<QQ", data, opt + 0x48, reserve, commit)
    open(path, "wb").write(data)
    print(f"patch_pe_stack: {path} stack reserve=0x{reserve:x} "
          f"commit=0x{commit:x}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print("usage: patch_pe_stack.py <ape_binary> <reserve> <commit>",
              file=sys.stderr)
        sys.exit(2)
    sys.exit(main(sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0)))
