#!/usr/bin/env python3
"""Guard the official build from unfinished compiler/IR/VM modules."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
MAKEFILE = (ROOT / "Makefile").read_text(encoding="utf-8")

EXPERIMENTAL = {
    "compiler.c", "ir.c", "semantic.c", "gc.c", "assembler.c",
    "instructions.c", "module.c", "arena.c", "temp_scope.c", "forest.c",
}
CANONICAL = {
    "lexer.c", "parser.c", "ast.c", "language_semantic.c", "language_runtime.c",
    "canonical_compiler.c", "canonical_ir.c", "typed_bytecode.c", "table.c", "dataset.c",
}
REFERENCE_VM = "vm.c"
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
if REFERENCE_VM in sources:
    errors.append("la VM interna de referencia no debe enlazarse en el binario oficial")
if not (ROOT / "src" / REFERENCE_VM).is_file():
    errors.append("falta la VM interna de referencia probada sobre bytecode verificado")
if (ROOT / "src" / "typed_vm.c").exists():
    errors.append("no debe existir una segunda implementación typed_vm.c")
if (ROOT / "src" / REFERENCE_VM).is_file():
    reference_source = (ROOT / "src" / REFERENCE_VM).read_text(encoding="utf-8")
    vm_header = (ROOT / "include" / "vm.h").read_text(encoding="utf-8")
    if not re.search(r'#include\s+[<"]vm\.h[>"]', reference_source):
        errors.append("el ejecutor debe pertenecer a la VM original")
    for signature in (
        r"bool\s+vm_init\s*\(\s*VirtualMachine\s*\*",
        r"bool\s+vm_run\s*\(\s*VirtualMachine\s*\*",
        r"void\s+vm_step\s*\(\s*VirtualMachine\s*\*",
        r"void\s+vm_destroy\s*\(\s*VirtualMachine\s*\*",
    ):
        if not re.search(signature, vm_header):
            errors.append("include/vm.h debe conservar el ciclo de vida de la VM original")
    if re.search(r"bool\s+vm_run\s*\(\s*const\s+uint8_t\s*\*", vm_header):
        errors.append("no debe quedar una segunda firma pública vm_run(bytecode, ...)")
    if not re.search(r"bool\s+vm_init_bytecode\s*\(\s*VirtualMachine\s*\*", vm_header):
        errors.append("el modo MLBC debe inicializar el mismo VirtualMachine original")
    if not re.search(r"vm_execute_instruction", reference_source):
        errors.append("la ruta de instrucciones de la VM original debe permanecer en src/vm.c")
    if not re.search(r"vm_execute_function", reference_source):
        errors.append("la ejecución MLBC debe integrarse en src/vm.c")
    if re.search(r'#include\s+[<"]compiler\.h[>"]', reference_source):
        errors.append("la VM no debe depender de la pila experimental compiler.h")
WINDOWS_PACKAGE = (ROOT / "packaging" / "windows" / "build.ps1")
if WINDOWS_PACKAGE.is_file():
    package_text = WINDOWS_PACKAGE.read_text(encoding="utf-8")
    source_list = re.search(r"\$SourceNames\s*=\s*@\((.*?)\n\)",
                            package_text, re.DOTALL)
    if source_list and REFERENCE_VM in source_list.group(1):
        errors.append("la VM canónica interna no debe incluirse en el paquete Windows")

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
