# Off-target unit tests -- build and run on the development PC, no hardware.
#
#   make -C tests          build and run every test (exit nonzero on failure)
#   make -C tests clean
#
# Only hardware-independent code (src/middleware, and src/app modules whose
# driver calls are faked here) can be tested this way -- that is what the
# layer rules in AGENTS.md "Source layout" are for.

ROOT    := ..
OUT     := $(ROOT)/build/tests
CC      ?= cc
CFLAGS  := -std=gnu11 -Wall -Wextra -Werror -g -I. -I$(ROOT)/src/drivers \
           -I$(ROOT)/src/middleware/scpi \
           -I$(ROOT)/src/app/settings -I$(ROOT)/src/config -I$(ROOT)/build/generated -I$(OUT)

SCPI    := $(ROOT)/src/middleware/scpi/scpi_parser.c
SCPI_UTIL := $(ROOT)/src/middleware/scpi/scpi_util.c
SETTINGS := $(ROOT)/src/app/settings
TESTS   := $(OUT)/test_scpi_parser $(OUT)/test_scpi_util $(OUT)/test_command_table \
           $(OUT)/test_settings_codec $(OUT)/test_profile_store

.PHONY: all run clean
all: run

run: $(TESTS)
	@set -e; for t in $(TESTS); do $$t; done

$(OUT):
	@mkdir -p $(OUT)

$(OUT)/test_scpi_parser: test_scpi_parser.c fake_uart.c $(SCPI) test_util.h fake_uart.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_scpi_parser.c fake_uart.c $(SCPI)

$(OUT)/test_scpi_util: test_scpi_util.c fake_uart.c $(SCPI_UTIL) $(ROOT)/src/middleware/scpi/scpi_util.h test_util.h fake_uart.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_scpi_util.c fake_uart.c $(SCPI_UTIL) -lm

# Every { "PATTERN", handler } row of the real command table, as C strings.
$(OUT)/command_patterns.inc: $(ROOT)/src/app/commands/command_table.c | $(OUT)
	sed -n 's/^[[:space:]]*{ *\("[^"]*"\),.*/\1,/p' $< > $@

$(OUT)/test_command_table: test_command_table.c known_short_collisions.inc $(OUT)/command_patterns.inc fake_uart.c $(SCPI) test_util.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_command_table.c fake_uart.c $(SCPI)

$(OUT)/test_settings_codec: test_settings_codec.c $(SETTINGS)/settings_codec.c $(SETTINGS)/settings_codec.h $(SCPI_UTIL) fake_uart.c test_util.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_settings_codec.c $(SETTINGS)/settings_codec.c $(SCPI_UTIL) fake_uart.c -lm

$(OUT)/test_profile_store: test_profile_store.c fake_nvstore.c fake_nvstore.h $(SETTINGS)/profile_store.c \
                           $(SETTINGS)/profile_store.h $(SETTINGS)/settings_codec.c test_util.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_profile_store.c fake_nvstore.c $(SETTINGS)/profile_store.c \
	    $(SETTINGS)/settings_codec.c $(SCPI_UTIL) fake_uart.c

clean:
	rm -rf $(OUT)
