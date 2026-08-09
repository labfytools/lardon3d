#!/usr/bin/env python3

import pathlib
import struct
import sys


def main() -> int:
    if len(sys.argv) != 3:
        return 2

    source = pathlib.Path(sys.argv[1]).read_bytes()
    if len(source) == 0 or len(source) % 4 != 0:
        return 1

    words = struct.unpack(f"<{len(source) // 4}I", source)
    output = pathlib.Path(sys.argv[2])
    lines = [
        "#ifndef LARDON3D_ORB_TOP2_SPV_H",
        "#define LARDON3D_ORB_TOP2_SPV_H",
        "",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        "static const uint32_t lardon3d_orb_top2_spv[] = {",
    ]
    for start in range(0, len(words), 6):
        chunk = ", ".join(f"0x{word:08x}U" for word in words[start : start + 6])
        lines.append(f"    {chunk},")
    lines.extend(
        [
            "};",
            "static const size_t lardon3d_orb_top2_spv_size =",
            "    sizeof(lardon3d_orb_top2_spv);",
            "",
            "#endif",
            "",
        ]
    )
    output.write_text("\n".join(lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
