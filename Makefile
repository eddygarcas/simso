# simso - modular build
#
#   make            release build  -> ./simso
#   make debug      -O0 -g, asserts
#   make asan       AddressSanitizer + UBSan
#   make filc       build with Fil-C (filcc) for full memory-safety checking
#   make run        build and run
#   make bench      build and run a 500-run parallel sweep
#   make clean

CC      ?= gcc
STD      = -std=gnu11
WARN     = -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes \
           -Wmissing-prototypes -Wno-unused-parameter
OPT     ?= -O2
CFLAGS  += $(STD) $(WARN) $(OPT) -pthread
LDFLAGS += -pthread
LDLIBS  += -lncursesw

SRC  = src/sim.c src/shared.c src/ui.c src/bench.c src/main.c
OBJ  = $(SRC:.c=.o)
DEP  = $(OBJ:.o=.d)
BIN  = simso

.PHONY: all debug asan filc run bench clean

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

debug:
	$(MAKE) clean
	$(MAKE) OPT="-O0 -g3 -fno-omit-frame-pointer"

asan:
	$(MAKE) clean
	$(MAKE) OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
	        LDFLAGS="-pthread -fsanitize=address,undefined"

# Fil-C builds go through build-filc.sh, which probes for filcc and for the
# ncurses variant its pizfix actually ships. Keeping that logic in one place
# rather than duplicating it here.
filc:
	./build-filc.sh

run: $(BIN)
	./$(BIN)

bench: $(BIN)
	./$(BIN) --bench 500

clean:
	rm -f $(OBJ) $(DEP) $(BIN) $(CHECK_BIN)
	rm -rf build

-include $(DEP)

# ---- tests ----------------------------------------------------------
CHECK_BIN = tests/check

$(CHECK_BIN): tests/check.c src/sim.c src/sim.h src/rng.h
	$(CC) $(STD) $(WARN) -O2 -Isrc -o $@ tests/check.c src/sim.c

.PHONY: check
check: $(CHECK_BIN)
	./$(CHECK_BIN)
