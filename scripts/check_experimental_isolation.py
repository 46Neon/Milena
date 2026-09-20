#!/usr/bin/env python3
"""Impide que las implementaciones experimentales entren al binario oficial."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
EXPERIMENTAL = {
    "arena.c", "assembler.c", "compiler.c", "forest.c", "gc.c",
    "instructions.c", "ir.c", "module.c", "semantic.c", "symbol.c",
    "temp_scope.c", "vm.c",
}


def main() -> int:
    makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
    sources = re.search(r"^SOURCES\s*=([\s\S]*?)^OBJECTS\s*=", makefile, re.MULTILINE)
    if not sources:
        print("ERROR: no se pudo localizar SOURCES en Makefile", file=sys.stderr)
        return 1
    linked = set(re.findall(r"src/([^\s\\]+\.c)", sources.group(1)))
    leaked = sorted(linked & EXPERIMENTAL)
    if leaked:
        print("ERROR: módulos experimentales enlazados en el producto: " + ", ".join(leaked), file=sys.stderr)
        return 1

    forbidden_headers = {name[:-2] + ".h" for name in EXPERIMENTAL}
    product_files = set()
    for path in (ROOT / "src").glob("*.c"):
        if path.name not in EXPERIMENTAL:
            product_files.add(path)
    errors = []
    for path in sorted(product_files):
        text = path.read_text(encoding="utf-8")
        for header in forbidden_headers:
            if f'#include "{header}"' in text:
                errors.append(f"{path.name}: incluye {header}")
    if errors:
        for error in errors:
            print("ERROR: " + error, file=sys.stderr)
        return 1
    print(f"OK: {len(EXPERIMENTAL)} módulos experimentales aislados del producto")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
