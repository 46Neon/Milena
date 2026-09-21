#!/usr/bin/env python3
"""Guard the single typed execution contract and backend boundary."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
header = (root/'include/execution_contract.h').read_text()
source = (root/'src/execution_contract.c').read_text()
make = (root/'Makefile').read_text()
for marker in ('MilenaExecutionPlan','MilenaExecutionReport','milena_stream_execute_plan','milena_table_execute_plan'):
    if marker not in header + source: raise SystemExit('missing canonical contract: '+marker)
if 'src/execution_contract.c' not in make: raise SystemExit('contract is not in product build')
if 'main(' in source: raise SystemExit('contract source contains a standalone runtime')
if 'sscanf' in source or 'system(' in source: raise SystemExit('contract bypasses typed execution')
print('OK: one typed execution contract feeds table and stream backends')
