#!/usr/bin/env python3
"""Guard the official build from unfinished compiler/IR/VM modules."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
MAKEFILE = (ROOT / "Makefile").read_text(encoding="utf-8")

EXPERIMENTAL = {
    "compiler.c", "ir.c", "vm.c", "semantic.c", "gc.c", "assembler.c",
    "instructions.c", "module.c", "arena.c", "temp_scope.c", "forest.c",
}
CANONICAL = {
    "lexer.c", "parser.c", "ast.c", "language_semantic.c", "language_runtime.c",
    "canonical_compiler.c", "canonical_ir.c", "typed_bytecode.c", "table.c", "dataset.c",
}
errors = []
# Capture the complete SOURCES assignment, including its continuation lines.
match = re.search(r"^SOURCES\s*=\s*(.*?)(?=^OBJECTS\s*=)", MAKEFILE, re.MULTILINE | re.DOTALL)
if not match:
    errors.append("Makefile no contiene una asignación SOURCES reconocible")
    sources = set()
else:
    sources = set(re.findall(r"src/([A-Za-z0-9_]+\.c)", match.group(1)))
for name in sorted(EXPERIMENTAL & sources):
    errors.append(f"módulo experimental enlazado en el binario oficial: {name}")
for name in sorted(CANONICAL - sources):
    errors.append(f"falta una fuente del pipeline canónico: {name}")

# The product sources may not accidentally pull the orphan compiler stack through
# a header include. Headers remain in the tree for future work, but are not active.
for path in sorted(ROOT.glob("src/*.c")):
    if path.name not in sources:
        continue
    text = path.read_text(encoding="utf-8")
    for header in ("compiler.h", "ir.h", "vm.h", "gc.h"):
        if re.search(rf'#include\s+[<\"]{re.escape(header)}[>\"]', text):
            errors.append(f"fuente oficial {path.name} incluye la pila experimental {header}")

if errors:
    print("\n".join(f"ERROR: {error}" for error in errors), file=sys.stderr)
    raise SystemExit(1)
print("Compiler/IR/VM boundary: OK (experimental modules excluded from official SOURCES)")
