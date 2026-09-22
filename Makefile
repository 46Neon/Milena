.DEFAULT_GOAL := all

CC ?= cc
# The canonical product is deliberately freestanding from libc implementation
# details: Termux supplies Clang/Bionic and the same source list is used there.
# Do not add glibc-only flags or Debian paths to this build contract.
TERMUX ?= 0
TERMUX_PREFIX ?= /data/data/com.termux/files/usr
TERMUX_CFLAGS ?= -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Oz -ffunction-sections -fdata-sections -Iinclude
TERMUX_LDFLAGS ?= -lm -Wl,--gc-sections
ifeq ($(TERMUX),1)
CFLAGS ?= $(TERMUX_CFLAGS)
LDFLAGS ?= $(TERMUX_LDFLAGS)
else
CFLAGS ?= -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -O2 -Iinclude
LDFLAGS ?= -lm
endif
SOURCES = src/common.c src/array.c src/table.c src/finance.c src/schema.c src/dataset.c src/analysis.c src/script.c src/main.c \
          src/lexer.c src/ast.c src/language_semantic.c src/parser.c src/symbol_table.c src/symbol.c src/language_runtime.c src/canonical_compiler.c src/interpreter.c \
          src/sst_dates.c src/sst_model.c src/sst_stats.c src/sst_histogram.c \
          src/sst_rates.c src/sst_report.c src/sst_report_advanced.c \
          src/sst_advanced.c src/sst_contingency.c src/sst_inference.c \
          src/sst_correlation.c src/sst_normality.c src/logger.c src/metrics.c src/spill.c src/stream.c src/entrypoints.c
OBJECTS = $(SOURCES:.c=.o)
SOURCES_NO_MAIN = $(filter-out src/main.c,$(SOURCES))
FUNCTION_OBJECTS = src/function_parser.o src/user_functions.o
TARGET = milena

.PHONY: all benchmark clean termux-build termux-install test check-termux-packaging check-termux-runner-contract check-termux-industrial check-compiler-boundary test-canonical-compiler test-sst test-array test-array-worker2 test-array-worker3 test-forest test-arena test-table test-table-worker4 test-pr21-regressions test-finance test-stream test-entrypoints test-language-array test-lexer-safety test-language-runtime test-parser-array test-parser-statistics test-parser-variables test-functions test-script-functions test-user-functions check-source-manifest check-experimental-isolation check-stream-architecture check-unification-architecture debug

test-array: tests/test_array
	./tests/test_array

test-array-worker2: tests/test_array_worker2
	./tests/test_array_worker2

tests/test_array_worker2: tests/test_array_worker2.c src/array.c src/common.c
	$(CC) $(CFLAGS) tests/test_array_worker2.c src/array.c src/common.c $(LDFLAGS) -o $@

test-array-worker3: tests/test_array_worker3
	./tests/test_array_worker3

tests/test_array_worker3: tests/test_array_worker3.c src/array.c src/common.c
	$(CC) $(CFLAGS) tests/test_array_worker3.c src/array.c src/common.c $(LDFLAGS) -o $@

test-forest: tests/test_forest
	./tests/test_forest

test-arena: tests/test_arena
	./tests/test_arena

tests/test_arena: tests/test_arena.c src/arena.c src/temp_scope.c src/common.c
	$(CC) $(CFLAGS) tests/test_arena.c src/arena.c src/temp_scope.c src/common.c $(LDFLAGS) -o $@

tests/test_forest: tests/test_forest.c src/forest.c src/array.c src/common.c
	$(CC) $(CFLAGS) tests/test_forest.c src/forest.c src/array.c src/common.c $(LDFLAGS) -o $@

tests/test_array: tests/test_array.c src/array.c src/common.c
	$(CC) $(CFLAGS) tests/test_array.c src/array.c src/common.c $(LDFLAGS) -o $@

test-table: tests/test_table
	./tests/test_table

tests/test_table: tests/test_table.c src/table.c src/array.c src/schema.c src/dataset.c src/common.c
	$(CC) $(CFLAGS) tests/test_table.c src/table.c src/array.c src/schema.c src/dataset.c src/common.c $(LDFLAGS) -o $@

test-table-worker4: tests/test_table_worker4
	./tests/test_table_worker4

tests/test_table_worker4: tests/test_table_worker4.c src/table.c src/array.c src/schema.c src/dataset.c src/common.c
	$(CC) $(CFLAGS) tests/test_table_worker4.c src/table.c src/array.c src/schema.c src/dataset.c src/common.c $(LDFLAGS) -o $@

test-pr21-regressions: tests/test_pr21_regressions
	./tests/test_pr21_regressions

tests/test_pr21_regressions: tests/test_pr21_regressions.c src/table.c src/array.c src/schema.c src/dataset.c src/common.c
	$(CC) $(CFLAGS) tests/test_pr21_regressions.c src/table.c src/array.c src/schema.c src/dataset.c src/common.c $(LDFLAGS) -o $@

test-stream: tests/test_stream
	./tests/test_stream

test-entrypoints: tests/test_entrypoints
	./tests/test_entrypoints

tests/test_entrypoints: tests/test_entrypoints.c $(SOURCES_NO_MAIN) $(FUNCTION_OBJECTS)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

tests/test_stream: tests/test_stream.c src/stream.c src/spill.c src/common.c
	$(CC) $(CFLAGS) tests/test_stream.c src/stream.c src/spill.c src/common.c $(LDFLAGS) -o $@

.PHONY: test-finance

test-finance: tests/test_finance
	./tests/test_finance

tests/test_finance: tests/test_finance.c src/finance.c src/common.c
	$(CC) $(CFLAGS) tests/test_finance.c src/finance.c src/common.c $(LDFLAGS) -o $@

.PHONY: test-language-array

test-language-array: tests/test_language_array
	./tests/test_language_array

tests/test_language_array: tests/test_language_array.c src/lexer.c src/ast.c src/common.c
	$(CC) $(CFLAGS) tests/test_language_array.c src/lexer.c src/ast.c src/common.c $(LDFLAGS) -o $@

test-lexer-safety: tests/test_lexer_safety
	./tests/test_lexer_safety

tests/test_lexer_safety: tests/test_lexer_safety.c src/lexer.c src/common.c
	$(CC) $(CFLAGS) tests/test_lexer_safety.c src/lexer.c src/common.c $(LDFLAGS) -o $@

test-language-runtime: tests/test_language_runtime
	timeout --signal=TERM --kill-after=5s 60s ./tests/test_language_runtime

tests/test_language_runtime: tests/test_language_runtime.c src/finance.c src/language_runtime.c src/language_semantic.c src/parser.c src/lexer.c src/ast.c src/symbol_table.c src/array.c src/dataset.c src/schema.c src/analysis.c src/table.c src/sst_advanced.c src/sst_histogram.c src/sst_normality.c src/sst_rates.c src/sst_inference.c src/sst_correlation.c src/sst_contingency.c src/sst_model.c src/common.c src/stream.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

.PHONY: test-parser-array

test-parser-array: tests/test_parser_array
	./tests/test_parser_array

tests/test_parser_array: tests/test_parser_array.c src/parser.c src/lexer.c src/ast.c src/common.c src/symbol_table.c
	$(CC) $(CFLAGS) tests/test_parser_array.c src/parser.c src/lexer.c src/ast.c src/common.c src/symbol_table.c $(LDFLAGS) -o $@

.PHONY: test-parser-variables

test-parser-variables: tests/test_parser_variables
	./tests/test_parser_variables

test-functions: tests/test_functions
	./tests/test_functions

tests/test_functions: tests/test_functions.c src/parser.c src/lexer.c src/ast.c src/interpreter.c src/symbol.c src/symbol_table.c src/dataset.c src/common.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

test-script-functions: tests/test_script_functions
	./tests/test_script_functions

tests/test_script_functions: tests/test_script_functions.c $(SOURCES_NO_MAIN) $(FUNCTION_OBJECTS)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

test-user-functions: tests/test_user_functions
	./tests/test_user_functions

tests/test_user_functions: tests/test_user_functions.c src/function_parser.c src/user_functions.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

tests/test_parser_variables: tests/test_parser_variables.c src/parser.c src/lexer.c src/ast.c src/common.c src/symbol_table.c
	$(CC) $(CFLAGS) tests/test_parser_variables.c src/parser.c src/lexer.c src/ast.c src/common.c src/symbol_table.c $(LDFLAGS) -o $@

test-parser-statistics: tests/test_parser_statistics
	./tests/test_parser_statistics

tests/test_parser_statistics: tests/test_parser_statistics.c src/parser.c src/lexer.c src/ast.c src/common.c src/symbol_table.c
	$(CC) $(CFLAGS) tests/test_parser_statistics.c src/parser.c src/lexer.c src/ast.c src/common.c src/symbol_table.c $(LDFLAGS) -o $@

SST_TEST_SOURCES = src/common.c src/sst_dates.c src/sst_model.c \
                   src/sst_stats.c src/sst_histogram.c src/sst_rates.c \
                   src/sst_report.c src/sst_report_advanced.c \
                   src/sst_advanced.c src/sst_contingency.c src/sst_inference.c \
                   src/sst_correlation.c src/sst_normality.c src/logger.c src/metrics.c

check-source-manifest:
	python3 scripts/check_source_manifest.py

check-experimental-isolation:
	python3 scripts/check_experimental_isolation.py

check-stream-architecture:
	python3 scripts/check_stream_architecture.py

check-unification-architecture:
	python3 scripts/check_unification_architecture.py

check-termux-packaging:
	python3 scripts/check_termux_packaging.py

check-termux-runner-contract:
	python3 scripts/check_termux_runner_contract.py

check-termux-industrial:
	python3 scripts/check_termux_industrial.py

check-compiler-boundary:
	python3 scripts/check_compiler_boundary.py

test-canonical-compiler: tests/test_canonical_compiler
	./tests/test_canonical_compiler

tests/test_canonical_compiler: tests/test_canonical_compiler.c src/canonical_compiler.c src/language_semantic.c src/parser.c src/lexer.c src/ast.c src/symbol_table.c src/table.c src/array.c src/dataset.c src/schema.c src/common.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

test-termux-packaging: check-termux-packaging
	python3 scripts/test_termux_packaging.py

all: $(TARGET)

# Reproducible compile/runtime measurements; see benchmarks/README.md.
benchmark:
	python3 benchmarks/benchmark.py

# Build targets consumed by the Termux recipe. They never build tests or the
# experimental compiler/IR/VM sources and never assume a Debian filesystem.
termux-build:
	$(MAKE) clean
	$(MAKE) TERMUX=1 CC="$${CC:-clang}" CFLAGS="$${CFLAGS:-$(TERMUX_CFLAGS)}" LDFLAGS="$${LDFLAGS:-$(TERMUX_LDFLAGS)}" all

termux-install: termux-build
	test -n "$(TERMUX_PREFIX)"
	install -Dm755 $(TARGET) "$(DESTDIR)$(TERMUX_PREFIX)/bin/$(TARGET)"
	install -Dm644 README.md "$(DESTDIR)$(TERMUX_PREFIX)/share/doc/milena/README.md"

$(TARGET): $(OBJECTS) $(FUNCTION_OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) $(FUNCTION_OBJECTS) $(LDFLAGS) -o $@

test-sst: tests/test_sst_modules
	./tests/test_sst_modules

tests/test_sst_modules: tests/test_sst_modules.c $(SST_TEST_SOURCES)
	$(CC) $(CFLAGS) tests/test_sst_modules.c $(SST_TEST_SOURCES) $(LDFLAGS) -o $@

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

debug:
	$(MAKE) clean
	$(MAKE) CFLAGS='-std=c17 -Wall -Wextra -Wpedantic -g3 -O0 -fsanitize=address,undefined -Iinclude' LDFLAGS='-fsanitize=address,undefined -lm'

test: check-source-manifest check-experimental-isolation check-stream-architecture check-unification-architecture check-termux-packaging check-termux-runner-contract check-compiler-boundary test-termux-packaging test-canonical-compiler $(TARGET) test-sst test-array test-array-worker2 test-array-worker3 test-forest test-arena test-table test-table-worker4 test-pr21-regressions test-finance test-stream test-entrypoints \
      test-language-array test-lexer-safety test-language-runtime test-parser-array test-parser-statistics \
      test-parser-variables test-functions test-script-functions test-user-functions
	./tests/run_tests.sh

clean:
	rm -f $(OBJECTS) $(FUNCTION_OBJECTS) $(TARGET) tests/test_sst_modules \
		tests/test_array tests/test_array_worker2 tests/test_array_worker3 tests/test_forest tests/test_arena tests/test_table tests/test_table_worker4 \
		tests/test_finance tests/test_pr21_regressions tests/test_stream tests/test_entrypoints tests/test_language_array tests/test_lexer_safety tests/test_language_runtime tests/test_parser_array \
		tests/test_parser_statistics tests/test_parser_variables tests/test_functions \
		tests/test_script_functions tests/test_user_functions tests/test_canonical_compiler reporte.json resultado.json



