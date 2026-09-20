CC ?= cc
CFLAGS ?= -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -O2 -Iinclude
LDFLAGS ?= -lm
SOURCES = src/common.c src/array.c src/table.c src/finance.c src/schema.c src/dataset.c src/analysis.c src/script.c src/main.c \
          src/sst_dates.c src/sst_model.c src/sst_stats.c src/sst_histogram.c \
          src/sst_rates.c src/sst_report.c src/sst_report_advanced.c \
          src/sst_advanced.c src/sst_contingency.c src/sst_inference.c \
          src/sst_correlation.c src/sst_normality.c src/logger.c src/metrics.c
OBJECTS = $(SOURCES:.c=.o)
TARGET = milena

.PHONY: all clean test test-sst test-array test-table test-finance test-language-array test-parser-array debug

test-array: tests/test_array
	./tests/test_array

test-forest: tests/test_forest
	./tests/test_forest

tests/test_forest: tests/test_forest.c src/forest.c src/array.c src/common.c
	$(CC) $(CFLAGS) tests/test_forest.c src/forest.c src/array.c src/common.c $(LDFLAGS) -o $@

tests/test_array: tests/test_array.c src/array.c src/common.c
	$(CC) $(CFLAGS) tests/test_array.c src/array.c src/common.c $(LDFLAGS) -o $@

test-table: tests/test_table
	./tests/test_table

tests/test_table: tests/test_table.c src/table.c src/array.c src/common.c
	$(CC) $(CFLAGS) tests/test_table.c src/table.c src/array.c src/common.c $(LDFLAGS) -o $@

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

.PHONY: test-parser-array

test-parser-array: tests/test_parser_array
	./tests/test_parser_array

tests/test_parser_array: tests/test_parser_array.c src/parser.c src/lexer.c src/ast.c src/common.c
	$(CC) $(CFLAGS) tests/test_parser_array.c src/parser.c src/lexer.c src/ast.c src/common.c $(LDFLAGS) -o $@

SST_TEST_SOURCES = src/common.c src/sst_dates.c src/sst_model.c \
                   src/sst_stats.c src/sst_histogram.c src/sst_rates.c \
                   src/sst_report.c src/sst_report_advanced.c \
                   src/sst_advanced.c src/sst_contingency.c src/sst_inference.c \
                   src/sst_correlation.c src/sst_normality.c src/logger.c src/metrics.c

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) $(LDFLAGS) -o $@

test-sst: tests/test_sst_modules
	./tests/test_sst_modules

tests/test_sst_modules: tests/test_sst_modules.c $(SST_TEST_SOURCES)
	$(CC) $(CFLAGS) tests/test_sst_modules.c $(SST_TEST_SOURCES) $(LDFLAGS) -o $@

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

debug:
	$(MAKE) clean
	$(MAKE) CFLAGS='-std=c17 -Wall -Wextra -Wpedantic -g3 -O0 -fsanitize=address,undefined -Iinclude' LDFLAGS='-fsanitize=address,undefined -lm'

test: $(TARGET) test-sst test-array test-forest test-table test-finance
	./tests/run_tests.sh

clean:
	rm -f $(OBJECTS) $(TARGET) tests/test_sst_modules tests/test_array tests/test_table reporte.json resultado.json
