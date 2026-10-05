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
           -I$(ROOT)/src/config -I$(ROOT)/src/app/control \
           -I$(ROOT)/src/app/protection -I$(ROOT)/build/generated -I$(OUT)

SCPI    := $(ROOT)/src/middleware/scpi/scpi_parser.c
TESTS   := $(OUT)/test_scpi_parser $(OUT)/test_command_table $(OUT)/test_state_machine $(OUT)/test_supply_mode $(OUT)/test_shot_profile $(OUT)/test_pid_core $(OUT)/test_feedback $(OUT)/test_target_profile $(OUT)/test_hvps_curve $(OUT)/test_closed_loop

.PHONY: all run clean
all: run

run: $(TESTS)
	@set -e; for t in $(TESTS); do $$t; done

$(OUT):
	@mkdir -p $(OUT)

$(OUT)/test_scpi_parser: test_scpi_parser.c fake_uart.c $(SCPI) test_util.h fake_uart.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_scpi_parser.c fake_uart.c $(SCPI)

# Every { "PATTERN", handler } row of the real command table, as C strings.
$(OUT)/command_patterns.inc: $(ROOT)/src/app/commands/command_table.c | $(OUT)
	sed -n 's/^[[:space:]]*{ *\("[^"]*"\),.*/\1,/p' $< > $@

$(OUT)/test_command_table: test_command_table.c $(OUT)/command_patterns.inc fake_uart.c $(SCPI) test_util.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_command_table.c fake_uart.c $(SCPI)

$(OUT)/test_state_machine: test_state_machine.c $(ROOT)/src/app/control/state_machine.c \
                           $(ROOT)/src/app/control/state_machine.h test_util.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_state_machine.c $(ROOT)/src/app/control/state_machine.c

$(OUT)/test_supply_mode: test_supply_mode.c $(ROOT)/src/app/control/supply_mode.c \
                         $(ROOT)/src/app/control/supply_mode.h test_util.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_supply_mode.c $(ROOT)/src/app/control/supply_mode.c

$(OUT)/test_shot_profile: test_shot_profile.c $(ROOT)/src/app/control/shot_profile.c $(ROOT)/src/app/control/shot_profile.h \
                          $(ROOT)/src/app/control/supply_mode.c test_util.h | $(OUT)
	$(CC) $(CFLAGS) -O2 -o $@ test_shot_profile.c $(ROOT)/src/app/control/shot_profile.c $(ROOT)/src/app/control/supply_mode.c

$(OUT)/test_pid_core: test_pid_core.c $(ROOT)/src/app/control/pid_core.c $(ROOT)/src/app/control/pid_core.h test_util.h | $(OUT)
	$(CC) $(CFLAGS) -O2 -o $@ test_pid_core.c $(ROOT)/src/app/control/pid_core.c -lm

$(OUT)/test_feedback: test_feedback.c $(ROOT)/src/app/control/feedback.c $(ROOT)/src/app/control/feedback.h test_util.h | $(OUT)
	$(CC) $(CFLAGS) -O2 -o $@ test_feedback.c $(ROOT)/src/app/control/feedback.c -lm

$(OUT)/test_target_profile: test_target_profile.c $(ROOT)/src/app/control/target_profile.c $(ROOT)/src/app/control/target_profile.h \
                            $(ROOT)/src/app/control/shot_profile.c test_util.h | $(OUT)
	$(CC) $(CFLAGS) -O2 -o $@ test_target_profile.c $(ROOT)/src/app/control/target_profile.c $(ROOT)/src/app/control/shot_profile.c $(ROOT)/src/app/control/supply_mode.c

$(OUT)/test_hvps_curve: test_hvps_curve.c $(ROOT)/src/app/control/hvps_curve.c $(ROOT)/src/app/control/hvps_curve.h test_util.h | $(OUT)
	$(CC) $(CFLAGS) -O2 -o $@ test_hvps_curve.c $(ROOT)/src/app/control/hvps_curve.c -lm

CL_SRC := $(ROOT)/src/app/control/hvps_curve.c $(ROOT)/src/app/control/closed_loop.c $(ROOT)/src/app/control/pid_core.c $(ROOT)/src/app/control/feedback.c \
          $(ROOT)/src/app/control/target_profile.c $(ROOT)/src/app/control/shot_profile.c $(ROOT)/src/app/control/supply_mode.c

$(OUT)/test_closed_loop: test_closed_loop.c $(CL_SRC) $(ROOT)/src/app/control/closed_loop.h test_util.h | $(OUT)
	$(CC) $(CFLAGS) -O2 -o $@ test_closed_loop.c $(CL_SRC) -lm

clean:
	rm -rf $(OUT)
