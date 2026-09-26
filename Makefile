.DEFAULT_GOAL := all

CC ?= cc
# The canonical product is deliberately freestanding from libc implementation
# details: Termux supplies Clang/Bionic and the same source list is used there.
# Do not add glibc-only flags or Debian paths to this build contract.
TERMUX ?= 0
TERMUX_PREFIX ?= /data/data/com.termux/files/usr
TERMUX_CFLAGS ?= -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Oz -ffunction-sections -fdata-sections -Iinclude
TERMUX_LDFLAGS ?= -lm -pthread -Wl,--gc-sections
ifeq ($(TERMUX),1)
CFLAGS ?= $(TERMUX_CFLAGS)
LDFLAGS ?= $(TERMUX_LDFLAGS)
else
CFLAGS ?= -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -O2 -Iinclude
LDFLAGS ?= -lm -pthread
endif
CPPFLAGS += -Iinclude -Ithird_party/nanoarrow/include -Ithird_party/sqlite
SQLITE_CFLAGS = -DSQLITE_THREADSAFE=1 -DSQLITE_DQS=0 -DSQLITE_OMIT_LOAD_EXTENSION
SOURCES = src/common.c src/array.c src/table.c src/finance.c src/schema.c src/dataset.c src/analysis.c src/script.c src/main.c \
          src/lexer.c src/ast.c src/language_semantic.c src/parser.c src/symbol_table.c src/symbol.c src/arrow_ipc.c src/language_runtime.c src/language_grouped_spill.c src/canonical_compiler.c src/canonical_ir.c src/typed_bytecode.c src/interpreter.c \
          src/sst_dates.c src/sst_model.c src/sst_stats.c src/sst_histogram.c \
          src/sst_rates.c src/sst_report.c src/sst_report_advanced.c \
          src/sst_advanced.c src/sst_contingency.c src/sst_inference.c \
          src/sst_correlation.c src/sst_normality.c src/logger.c src/metrics.c src/stream.c src/source_reader.c src/group_key_codec.c src/partition_plan.c src/partition_executor.c src/partition_reduce.c src/process_executor.c src/partition_protocol.c src/partition_protocol_reduce.c src/spill_store.c third_party/nanoarrow/src/nanoarrow.c third_party/nanoarrow/src/nanoarrow_ipc.c third_party/nanoarrow/src/flatcc.c src/mergeable_aggregate.c src/grouped_aggregate.c src/external_merge.c src/external_sort.c src/query_plan.c src/entrypoints.c src/sqlite_backend.c third_party/sqlite/sqlite3.c
OBJECTS = $(SOURCES:.c=.o)
SOURCES_NO_MAIN = $(filter-out src/main.c,$(SOURCES))
TEST_SOURCES_NO_MAIN = $(filter-out third_party/sqlite/sqlite3.c,$(SOURCES_NO_MAIN))
TEST_SQLITE_OBJECT = third_party/sqlite/sqlite3.o
FUNCTION_OBJECTS = src/function_parser.o src/user_functions.o
TARGET = milena

.PHONY: all benchmark benchmark-stream benchmark-stream-grouped benchmark-stream-grouped-spill clean termux-build termux-install termux-contract test check-termux-packaging check-termux-runner-contract check-termux-industrial check-markdown-links check-compiler-boundary test-termux-packaging test-canonical-compiler test-sst test-array test-array-worker2 test-array-worker3 test-forest test-arena test-table test-table-worker4 test-pr21-regressions test-finance test-stream test-partition-plan test-partition-executor test-partition-equivalence test-partition-concurrency test-partition-reduce test-partition-budget test-process-executor test-partition-protocol test-protocol-reduce test-spill-store test-mergeable-aggregate test-grouped-aggregate test-external-merge test-external-sort test-query-plan test-grouped-stream-spill-runtime test-data-engine-parity test-entrypoints test-language-array test-lexer-safety test-language-runtime test-parser-array test-parser-statistics test-parser-variables test-functions test-script-functions test-user-functions test-group-key-codec test-arrow-ipc test-common-tokenizer test-ast-validation check-source-manifest check-experimental-isolation check-stream-architecture check-unification-architecture check-hir-ast-coverage debug

.PHONY: test-common-tokenizer
test-common-tokenizer: tests/test_common_tokenizer
	./tests/test_common_tokenizer

tests/test_common_tokenizer: tests/test_common_tokenizer.c src/common.c include/common.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< src/common.c $(LDFLAGS) -o $@

test-array: tests/test_array
	./tests/test_array

test-array-worker2: tests/test_array_worker2
	./tests/test_array_worker2

tests/test_array_worker2: tests/test_array_worker2.c src/array.c src/common.c
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_array_worker2.c src/array.c src/common.c $(LDFLAGS) -o $@

test-array-worker3: tests/test_array_worker3
	./tests/test_array_worker3

tests/test_array_worker3: tests/test_array_worker3.c src/array.c src/common.c
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_array_worker3.c src/array.c src/common.c $(LDFLAGS) -o $@

test-forest: tests/test_forest
	./tests/test_forest

test-arena: tests/test_arena
	./tests/test_arena

tests/test_arena: tests/test_arena.c src/arena.c src/temp_scope.c src/common.c
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_arena.c src/arena.c src/temp_scope.c src/common.c $(LDFLAGS) -o $@

tests/test_forest: tests/test_forest.c src/forest.c src/array.c src/common.c
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_forest.c src/forest.c src/array.c src/common.c $(LDFLAGS) -o $@

tests/test_array: tests/test_array.c src/array.c src/common.c
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_array.c src/array.c src/common.c $(LDFLAGS) -o $@

test-table: tests/test_table
	./tests/test_table

tests/test_table: tests/test_table.c src/table.c src/array.c src/schema.c src/dataset.c src/common.c
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_table.c src/table.c src/array.c src/schema.c src/dataset.c src/common.c $(LDFLAGS) -o $@

test-table-worker4: tests/test_table_worker4
	./tests/test_table_worker4

tests/test_table_worker4: tests/test_table_worker4.c src/table.c src/array.c src/schema.c src/dataset.c src/common.c
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_table_worker4.c src/table.c src/array.c src/schema.c src/dataset.c src/common.c $(LDFLAGS) -o $@

test-pr21-regressions: tests/test_pr21_regressions
	./tests/test_pr21_regressions

tests/test_pr21_regressions: tests/test_pr21_regressions.c src/table.c src/array.c src/schema.c src/dataset.c src/common.c
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_pr21_regressions.c src/table.c src/array.c src/schema.c src/dataset.c src/common.c $(LDFLAGS) -o $@

test-stream: tests/test_stream
	./tests/test_stream

test-entrypoints: tests/test_entrypoints
	./tests/test_entrypoints

tests/test_entrypoints: tests/test_entrypoints.c $(TEST_SOURCES_NO_MAIN) $(FUNCTION_OBJECTS) $(TEST_SQLITE_OBJECT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SQLITE_CFLAGS) $(filter %.c,$^) $(filter %.o,$^) $(LDFLAGS) -o $@

tests/test_stream: tests/test_stream.c src/stream.c src/source_reader.c src/group_key_codec.c src/grouped_aggregate.c src/mergeable_aggregate.c src/spill_store.c src/common.c
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_stream.c src/stream.c src/source_reader.c src/group_key_codec.c src/grouped_aggregate.c src/mergeable_aggregate.c src/spill_store.c src/common.c $(LDFLAGS) -o $@

test-partition-plan: tests/test_partition_plan
	./tests/test_partition_plan

tests/test_partition_plan: tests/test_partition_plan.c src/partition_plan.c src/common.c
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_partition_plan.c src/partition_plan.c src/common.c $(LDFLAGS) -o $@

test-partition-executor: tests/test_partition_executor
	./tests/test_partition_executor

tests/test_partition_executor: tests/test_partition_executor.c src/partition_executor.c src/partition_plan.c src/common.c
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_partition_executor.c src/partition_executor.c src/partition_plan.c src/common.c $(LDFLAGS) -o $@

test-partition-equivalence: tests/test_partition_equivalence
	./tests/test_partition_equivalence

tests/test_partition_equivalence: tests/test_partition_equivalence.c src/partition_executor.c src/partition_plan.c src/common.c
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_partition_equivalence.c src/partition_executor.c src/partition_plan.c src/common.c $(LDFLAGS) -o $@

test-partition-concurrency: tests/test_partition_concurrency
	./tests/test_partition_concurrency

tests/test_partition_concurrency: tests/test_partition_concurrency.c src/partition_executor.c src/partition_plan.c src/common.c
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_partition_concurrency.c src/partition_executor.c src/partition_plan.c src/common.c $(LDFLAGS) -o $@

test-partition-reduce: tests/test_partition_reduce
	./tests/test_partition_reduce

tests/test_partition_reduce: tests/test_partition_reduce.c src/partition_reduce.c src/partition_plan.c src/common.c
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_partition_reduce.c src/partition_reduce.c src/partition_plan.c src/common.c $(LDFLAGS) -o $@

test-partition-budget: tests/test_partition_budget
	./tests/test_partition_budget

tests/test_partition_budget: tests/test_partition_budget.c src/partition_executor.c src/partition_plan.c src/common.c
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_partition_budget.c src/partition_executor.c src/partition_plan.c src/common.c $(LDFLAGS) -o $@

test-partition-protocol: tests/test_partition_protocol
	./tests/test_partition_protocol

tests/test_partition_protocol: tests/test_partition_protocol.c src/partition_protocol.c src/common.c
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_partition_protocol.c src/partition_protocol.c src/common.c $(LDFLAGS) -o $@

test-protocol-reduce: tests/test_protocol_reduce
	./tests/test_protocol_reduce

tests/test_protocol_reduce: tests/test_protocol_reduce.c src/partition_protocol_reduce.c src/partition_protocol.c src/partition_reduce.c src/partition_plan.c src/common.c
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_protocol_reduce.c src/partition_protocol_reduce.c src/partition_protocol.c src/partition_reduce.c src/partition_plan.c src/common.c $(LDFLAGS) -o $@

test-spill-store: tests/test_spill_store
	./tests/test_spill_store

test-group-key-codec: tests/test_group_key_codec
	./tests/test_group_key_codec

tests/test_group_key_codec: tests/test_group_key_codec.c src/group_key_codec.c src/common.c include/group_key_codec.h
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_group_key_codec.c src/group_key_codec.c src/common.c $(LDFLAGS) -o $@

tests/test_spill_store: tests/test_spill_store.c src/spill_store.c src/common.c
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_spill_store.c src/spill_store.c src/common.c $(LDFLAGS) -o $@

test-mergeable-aggregate: tests/test_mergeable_aggregate
	./tests/test_mergeable_aggregate

tests/test_mergeable_aggregate: tests/test_mergeable_aggregate.c src/mergeable_aggregate.c src/spill_store.c src/common.c
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_mergeable_aggregate.c src/mergeable_aggregate.c src/spill_store.c src/common.c $(LDFLAGS) -o $@

test-query-plan: tests/test_query_plan
	./tests/test_query_plan

test-grouped-stream-spill-runtime: $(TARGET)
	sh tests/test_grouped_stream_spill_runtime.sh

test-data-engine-parity: $(TARGET)
	sh tests/test_data_engine_parity.sh

tests/test_query_plan: tests/test_query_plan.c src/query_plan.c src/ast.c src/common.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDFLAGS) -o $@

test-grouped-aggregate: tests/test_grouped_aggregate
	./tests/test_grouped_aggregate

tests/test_grouped_aggregate: tests/test_grouped_aggregate.c src/grouped_aggregate.c src/mergeable_aggregate.c src/spill_store.c src/common.c include/grouped_aggregate.h include/mergeable_aggregate.h include/spill_store.h
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_grouped_aggregate.c src/grouped_aggregate.c src/mergeable_aggregate.c src/spill_store.c src/common.c $(LDFLAGS) -o $@

test-external-merge: tests/test_external_merge
	./tests/test_external_merge

tests/test_external_merge: tests/test_external_merge.c src/external_merge.c src/spill_store.c src/common.c
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_external_merge.c src/external_merge.c src/spill_store.c src/common.c $(LDFLAGS) -o $@

test-external-sort: tests/test_external_sort
	./tests/test_external_sort

tests/test_external_sort: tests/test_external_sort.c src/external_sort.c src/external_merge.c src/spill_store.c src/common.c
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_external_sort.c src/external_sort.c src/external_merge.c src/spill_store.c src/common.c $(LDFLAGS) -o $@

test-process-executor: tests/test_process_executor
	./tests/test_process_executor

tests/test_process_executor: tests/test_process_executor.c src/process_executor.c src/partition_executor.c src/partition_plan.c src/common.c
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_process_executor.c src/process_executor.c src/partition_executor.c src/partition_plan.c src/common.c $(LDFLAGS) -o $@

.PHONY: test-finance

test-finance: tests/test_finance
	./tests/test_finance

tests/test_finance: tests/test_finance.c src/finance.c src/common.c
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_finance.c src/finance.c src/common.c $(LDFLAGS) -o $@

.PHONY: test-language-array

test-language-array: tests/test_language_array
	./tests/test_language_array

tests/test_language_array: tests/test_language_array.c src/lexer.c src/ast.c src/common.c
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_language_array.c src/lexer.c src/ast.c src/common.c $(LDFLAGS) -o $@

test-lexer-safety: tests/test_lexer_safety
	./tests/test_lexer_safety

tests/test_lexer_safety: tests/test_lexer_safety.c src/lexer.c src/common.c
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_lexer_safety.c src/lexer.c src/common.c $(LDFLAGS) -o $@

test-language-runtime: tests/test_language_runtime
	timeout --signal=TERM --kill-after=5s 60s ./tests/test_language_runtime

tests/test_language_runtime: tests/test_language_runtime.c src/finance.c src/language_runtime.c src/canonical_compiler.c src/canonical_ir.c src/language_semantic.c src/parser.c src/lexer.c src/ast.c src/symbol_table.c src/array.c src/dataset.c src/schema.c src/analysis.c src/table.c src/arrow_ipc.c third_party/nanoarrow/src/nanoarrow.c third_party/nanoarrow/src/nanoarrow_ipc.c third_party/nanoarrow/src/flatcc.c src/sst_advanced.c src/sst_histogram.c src/sst_normality.c src/sst_rates.c src/sst_inference.c src/sst_correlation.c src/sst_contingency.c src/sst_model.c src/common.c src/stream.c src/source_reader.c src/group_key_codec.c src/language_grouped_spill.c src/grouped_aggregate.c src/mergeable_aggregate.c src/spill_store.c src/query_plan.c src/sqlite_backend.c $(TEST_SQLITE_OBJECT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SQLITE_CFLAGS) $(filter %.c,$^) $(filter %.o,$^) $(LDFLAGS) -o $@

.PHONY: test-arrow-ipc
test-arrow-ipc: tests/test_arrow_ipc
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/check_arrow_ipc_fixtures.c tools/milena_sha256.c $(LDFLAGS) -o tests/check_arrow_ipc_fixtures && ./tests/check_arrow_ipc_fixtures tests/fixtures/arrow_ipc && ./tests/test_arrow_ipc

tests/test_arrow_ipc: tests/test_arrow_ipc.c src/arrow_ipc.c src/common.c third_party/nanoarrow/src/nanoarrow.c third_party/nanoarrow/src/nanoarrow_ipc.c third_party/nanoarrow/src/flatcc.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDFLAGS) -o $@

.PHONY: test-sqlite-backend test-sqlite-cli test-sqlite-typed-sql
test-sqlite-backend: tests/test_sqlite_backend
	./tests/test_sqlite_backend

tests/test_sqlite_backend: tests/test_sqlite_backend.c src/sqlite_backend.c src/query_plan.c src/ast.c src/table.c src/array.c src/schema.c src/dataset.c src/common.c third_party/sqlite/sqlite3.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SQLITE_CFLAGS) $(filter %.c,$^) third_party/sqlite/sqlite3.o $(LDFLAGS) -o $@

src/sqlite_backend.o: src/sqlite_backend.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SQLITE_CFLAGS) -c $< -o $@

test-sqlite-cli: $(TARGET)
	bash tests/test_sqlite_cli.sh

test-sqlite-typed-sql: tests/test_sqlite_typed_sql
	./tests/test_sqlite_typed_sql

tests/test_sqlite_typed_sql: tests/test_sqlite_typed_sql.c src/parser.c src/lexer.c src/ast.c src/query_plan.c src/common.c src/symbol_table.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c,$^) $(LDFLAGS) -o $@

.PHONY: test-parser-array

test-parser-array: tests/test_parser_array
	./tests/test_parser_array

tests/test_parser_array: tests/test_parser_array.c src/parser.c src/lexer.c src/ast.c src/common.c src/symbol_table.c
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_parser_array.c src/parser.c src/lexer.c src/ast.c src/common.c src/symbol_table.c $(LDFLAGS) -o $@

.PHONY: test-parser-variables test-ast-validation

test-parser-variables: tests/test_parser_variables
	./tests/test_parser_variables

test-ast-validation: tests/test_ast_validation
	./tests/test_ast_validation

tests/test_ast_validation: tests/test_ast_validation.c src/parser.c src/lexer.c src/ast.c src/common.c src/symbol_table.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

test-functions: tests/test_functions
	./tests/test_functions

tests/test_functions: tests/test_functions.c src/parser.c src/lexer.c src/ast.c src/interpreter.c src/symbol.c src/symbol_table.c src/dataset.c src/common.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDFLAGS) -o $@

test-script-functions: tests/test_script_functions
	./tests/test_script_functions

tests/test_script_functions: tests/test_script_functions.c $(TEST_SOURCES_NO_MAIN) $(FUNCTION_OBJECTS) $(TEST_SQLITE_OBJECT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SQLITE_CFLAGS) $(filter %.c,$^) $(filter %.o,$^) $(LDFLAGS) -o $@

test-user-functions: tests/test_user_functions
	./tests/test_user_functions

tests/test_user_functions: tests/test_user_functions.c src/function_parser.c src/user_functions.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDFLAGS) -o $@

tests/test_parser_variables: tests/test_parser_variables.c src/parser.c src/lexer.c src/ast.c src/common.c src/symbol_table.c
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_parser_variables.c src/parser.c src/lexer.c src/ast.c src/common.c src/symbol_table.c $(LDFLAGS) -o $@

test-parser-statistics: tests/test_parser_statistics
	./tests/test_parser_statistics

tests/test_parser_statistics: tests/test_parser_statistics.c src/parser.c src/lexer.c src/ast.c src/common.c src/symbol_table.c
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_parser_statistics.c src/parser.c src/lexer.c src/ast.c src/common.c src/symbol_table.c $(LDFLAGS) -o $@

SST_TEST_SOURCES = src/common.c src/sst_dates.c src/sst_model.c \
                   src/sst_stats.c src/sst_histogram.c src/sst_rates.c \
                   src/sst_report.c src/sst_report_advanced.c \
                   src/sst_advanced.c src/sst_contingency.c src/sst_inference.c \
                   src/sst_correlation.c src/sst_normality.c src/logger.c src/metrics.c

check-markdown-links: tools/check_repository_contracts
	./tools/check_repository_contracts markdown-links $(shell find . -type f -name '*.md' -not -path './.git/*' | sort)

check-source-manifest: tools/check_repository_contracts
	./tools/check_repository_contracts source-manifest $(wildcard src/*.c)

check-experimental-isolation: tools/check_repository_contracts
	./tools/check_repository_contracts experimental-isolation $(wildcard src/*.c)

check-stream-architecture: check-source-manifest tools/check_repository_contracts
	./tools/check_repository_contracts stream-architecture

check-unification-architecture: check-hir-ast-coverage
	./tools/check_architecture architecture

check-termux-packaging: tools/check_repository_contracts
	./tools/check_repository_contracts termux-packaging

check-termux-runner-contract: tools/check_repository_contracts
	./tools/check_repository_contracts termux-runner-contract

check-termux-industrial: tools/check_repository_contracts
	./tools/check_repository_contracts termux-industrial

check-compiler-boundary: tools/check_repository_contracts
	./tools/check_repository_contracts compiler-boundary $(wildcard src/*.c)

tools/check_repository_contracts: tools/check_repository_contracts.c tools/milena_sha256.c tools/milena_sha256.h
	$(CC) $(CFLAGS) -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion tools/check_repository_contracts.c tools/milena_sha256.c -o $@

check-hir-ast-coverage:
	$(CC) $(CFLAGS) -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion tools/check_architecture.c -o tools/check_architecture && ./tools/check_architecture hir

test-typed-bytecode: tests/test_typed_bytecode
	./tests/test_typed_bytecode

.PHONY: test-typed-bytecode
tests/test_typed_bytecode: tests/test_typed_bytecode.c src/vm.c src/gc.c src/typed_bytecode.c src/canonical_ir.c src/dataset.c src/table.c src/array.c src/schema.c src/common.c include/vm.h include/typed_bytecode.h include/typed_ir.h include/ir.h include/dataset.h include/gc.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c,$^) $(LDFLAGS) -o $@

test-canonical-compiler: check-hir-ast-coverage tests/test_canonical_compiler
	./tests/test_canonical_compiler

tests/test_canonical_compiler: tests/test_canonical_compiler.c src/canonical_compiler.c src/query_plan.c src/language_semantic.c src/parser.c src/lexer.c src/ast.c src/symbol_table.c src/table.c src/array.c src/dataset.c src/schema.c src/common.c src/gc.c src/canonical_ir.c src/typed_bytecode.c src/vm.c include/canonical_compiler.h include/query_plan.h include/vm.h include/typed_bytecode.h include/typed_ir.h include/ir.h include/dataset.h include/gc.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c,$^) $(LDFLAGS) -o $@

test-termux-packaging: check-termux-packaging tools/validate_termux_elf tests/test_termux_packaging
	./tests/test_termux_packaging

tools/validate_termux_elf: tools/validate_termux_elf.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion $< $(LDFLAGS) -o $@

tests/test_termux_packaging: tests/test_termux_packaging.c tools/milena_sha256.c tools/milena_sha256.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion tests/test_termux_packaging.c tools/milena_sha256.c $(LDFLAGS) -o $@

all: $(TARGET)

# Reproducible compile/runtime measurements; see benchmarks/README.md.
benchmark: benchmarks/benchmark
	@status=0; ./benchmarks/benchmark $(BENCHMARK_ARGS) || status=$$?; rm -f benchmarks/benchmark; exit $$status

benchmarks/benchmark: benchmarks/benchmark.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion $< $(LDFLAGS) -o $@

# Small and medium deterministic CSV workloads; large runs require --large-rows.
benchmark-stream: $(TARGET)
	python3 benchmarks/stream_benchmark.py

# Bounded deterministic grouped-stream workloads; large runs require --large-rows.
benchmark-stream-grouped: $(TARGET) benchmarks/grouped_stream_benchmark
	@status=0; ./benchmarks/grouped_stream_benchmark $(BENCHMARK_STREAM_GROUPED_ARGS) || status=$$?; rm -f benchmarks/grouped_stream_benchmark; exit $$status

benchmarks/grouped_stream_benchmark: benchmarks/grouped_stream_benchmark.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion $< $(LDFLAGS) -o $@

# Small deterministic memory-vs-spill check; large runs require --large-rows.
benchmark-stream-grouped-spill: $(TARGET)
	python3 benchmarks/grouped_spill_benchmark.py --repetitions 1

# Build targets consumed by the Termux recipe. They never build tests or the
# experimental compiler/IR/VM sources and never assume a Debian filesystem.
termux-build:
	$(MAKE) clean
	$(MAKE) TERMUX=1 CC="$${CC:-clang}" CFLAGS="$${CFLAGS:-$(TERMUX_CFLAGS)}" LDFLAGS="$${LDFLAGS:-$(TERMUX_LDFLAGS)}" all tools/validate_termux_elf

termux-install: termux-build
	test -n "$(TERMUX_PREFIX)"
	install -Dm755 $(TARGET) "$(DESTDIR)$(TERMUX_PREFIX)/bin/$(TARGET)"
	install -Dm644 README.md "$(DESTDIR)$(TERMUX_PREFIX)/share/doc/milena/README.md"
	install -Dm644 LICENSE "$(DESTDIR)$(TERMUX_PREFIX)/share/licenses/milena/LICENSE"
	install -Dm644 third_party/nanoarrow/LICENSE.txt "$(DESTDIR)$(TERMUX_PREFIX)/share/licenses/milena/nanoarrow-LICENSE.txt"
	install -Dm644 third_party/nanoarrow/NOTICE.txt "$(DESTDIR)$(TERMUX_PREFIX)/share/licenses/milena/nanoarrow-NOTICE.txt"
	install -Dm644 third_party/nanoarrow/FLATCC-LICENSE.txt "$(DESTDIR)$(TERMUX_PREFIX)/share/licenses/milena/flatcc-LICENSE.txt"
	install -Dm644 third_party/sqlite/README.md "$(DESTDIR)$(TERMUX_PREFIX)/share/licenses/milena/sqlite-PROVENANCE-LICENSE.md"

# Host-side, reproducible contract. It checks the canonical binary and CLI
# without pretending that a Linux runner is Android/Bionic hardware.
termux-contract:
	bash scripts/termux-native-contract.sh

$(TARGET): $(OBJECTS) $(FUNCTION_OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OBJECTS) $(FUNCTION_OBJECTS) $(LDFLAGS) -o $@

third_party/sqlite/%.o: third_party/sqlite/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -w $(SQLITE_CFLAGS) -c $< -o $@

test-sst: tests/test_sst_modules
	./tests/test_sst_modules

tests/test_sst_modules: tests/test_sst_modules.c $(SST_TEST_SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_sst_modules.c $(SST_TEST_SOURCES) $(LDFLAGS) -o $@

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

debug:
	$(MAKE) clean
	$(MAKE) CFLAGS='-std=c17 -Wall -Wextra -Wpedantic -g3 -O0 -fsanitize=address,undefined -Iinclude' LDFLAGS='-fsanitize=address,undefined -lm'

test: check-source-manifest check-experimental-isolation check-stream-architecture check-unification-architecture check-termux-packaging check-termux-runner-contract check-termux-industrial check-compiler-boundary test-termux-packaging benchmarks/benchmark test-canonical-compiler test-typed-bytecode benchmark-stream benchmark-stream-grouped benchmark-stream-grouped-spill $(TARGET) test-sst test-array test-array-worker2 test-array-worker3 test-forest test-arena test-table test-table-worker4 test-pr21-regressions test-finance test-stream test-partition-plan test-partition-executor test-partition-equivalence test-partition-concurrency test-partition-reduce test-partition-budget test-process-executor test-spill-store test-group-key-codec test-mergeable-aggregate test-grouped-aggregate test-external-merge test-external-sort test-query-plan test-grouped-stream-spill-runtime test-data-engine-parity test-entrypoints test-common-tokenizer test-ast-validation test-language-array test-lexer-safety test-language-runtime test-arrow-ipc test-parser-array test-parser-statistics test-parser-variables test-functions test-script-functions test-user-functions test-sqlite-backend test-sqlite-typed-sql test-sqlite-cli
	./tests/run_tests.sh
	./benchmarks/benchmark --help && rm -f benchmarks/benchmark

clean:
	rm -f $(OBJECTS) $(FUNCTION_OBJECTS) $(TARGET) tools/check_architecture tools/check_repository_contracts tools/validate_termux_elf tests/test_sst_modules \
		tests/test_array tests/test_array_worker2 tests/test_array_worker3 tests/test_forest tests/test_arena tests/test_table tests/test_table_worker4 \
		tests/test_finance tests/test_pr21_regressions tests/test_stream tests/test_partition_plan tests/test_partition_executor tests/test_partition_equivalence tests/test_partition_concurrency tests/test_partition_reduce tests/test_partition_budget tests/test_process_executor tests/test_partition_protocol tests/test_protocol_reduce tests/test_spill_store tests/test_group_key_codec tests/test_mergeable_aggregate tests/test_grouped_aggregate tests/test_external_merge tests/test_external_sort tests/test_query_plan tests/test_entrypoints tests/test_language_array tests/test_lexer_safety tests/test_language_runtime tests/test_parser_array tests/test_parser_statistics tests/test_parser_variables tests/test_ast_validation tests/test_functions tests/test_script_functions tests/test_user_functions tests/test_arrow_ipc tests/check_arrow_ipc_fixtures tests/test_common_tokenizer tests/test_canonical_compiler tests/test_typed_bytecode tests/test_termux_packaging benchmarks/grouped_stream_benchmark tests/test_sqlite_backend tests/test_sqlite_typed_sql tests/arrow-primitive-output.stream tests/arrow-text-output.stream tests/arrow-wide-output.stream tests/arrow-failure-destination.stream tests/arrow-truncated.stream reporte.json resultado.json

# Opt-in end-to-end million-row validation; it remains outside ordinary make test.
.PHONY: scale-million-row
scale-million-row: $(TARGET)
	python3 benchmarks/million_row_validation.py $(if $(SCALE_RESULT),--output $(SCALE_RESULT),)
