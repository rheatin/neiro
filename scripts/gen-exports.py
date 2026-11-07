#!/usr/bin/env python3
"""Generate the Mach-O export list from nae.h, intersected with what is defined.

    python scripts/gen-exports.py engine/nae.h engine/nae.exports.txt [nm-output]

Why the intersection: `nae.h` is the contract, but not every declared function is
defined on every platform (`NAE_QueryPeaks` is not). A list that names a symbol
the library does not define makes the linker fail; a list that omits one the
library *does* define makes the symbol disappear from the export table and the
host fails at load. Neither is a build error the first time, so the list is
generated rather than hand-written and the gap is printed.

`nm-output` is the output of `nm -gU build-macos/libnae.dylib` (or `nm` on the
object files). Without it the script emits every declaration and says so.
"""
import re
import sys

header_path, out_path = sys.argv[1], sys.argv[2]
nm_path = sys.argv[3] if len(sys.argv) > 3 else None

declared = sorted(set(re.findall(r"\b(NAE_[A-Za-z0-9_]+)\s*\(", open(header_path, encoding="utf-8").read())))

defined = None
if nm_path:
    defined = set()
    for line in open(nm_path, encoding="utf-8"):
        m = re.search(r"\bT _([A-Za-z0-9_]+)$", line.strip())
        if m:
            defined.add(m.group(1))

if defined is None:
    print(f"warning: no nm output given; writing all {len(declared)} declarations, "
          f"which will fail to link if any is undefined on this platform", file=sys.stderr)
    chosen = declared
else:
    chosen = [n for n in declared if n in defined]
    missing = [n for n in declared if n not in defined]
    extra = sorted(defined - set(declared))
    if missing:
        print(f"declared but not defined here ({len(missing)}): {', '.join(missing)}",
              file=sys.stderr)
    if extra:
        print(f"defined but not declared in the header ({len(extra)}): {', '.join(extra)}",
              file=sys.stderr)

with open(out_path, "w", encoding="utf-8") as out:
    for n in chosen:
        out.write("_" + n + "\n")
print(f"{len(chosen)} of {len(declared)} declared symbols -> {out_path}")
