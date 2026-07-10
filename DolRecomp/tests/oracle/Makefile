ifneq ($(wildcard /opt/devkitpro),)
DEVKITPRO ?= /opt/devkitpro
else
DEVKITPRO ?= C:/devkitPro
endif
DEVKITPPC ?= $(DEVKITPRO)/devkitPPC
LIBOGC    ?= $(DEVKITPRO)/libogc

PREFIX  := $(DEVKITPPC)/bin/powerpc-eabi-
CC      := $(PREFIX)gcc
HOST_CC ?= cc
ELF2DOL := $(DEVKITPRO)/tools/bin/elf2dol

MACHDEP := -DGEKKO -mogc -mcpu=750 -meabi -mhard-float
CFLAGS  := $(MACHDEP) -O0 -Wall -Wextra -g -I$(LIBOGC)/include
LDFLAGS := $(MACHDEP) -L$(LIBOGC)/lib/cube -Wl,-Map,oracle.map
LIBS    := -lfat -logc -lm

all: oracle.dol

generate_cases: generate_cases.c ../diff/probe.h ../diff/probes.inc \
		../opcode_samples.h ../test_codegen_emit.c
	$(HOST_CC) -std=c11 -Wall -Wextra -I../../src -I../diff -I.. \
		-DDOLRECOMP_OPCODE_SAMPLES_ONLY generate_cases.c \
		../../tests/test_codegen_emit.c ../../src/frontend/decoder.c -o $@

generated_cases.inc coverage.json: generate_cases
	./generate_cases generated_cases.inc coverage.json

oracle.elf: source/main.c source/runner.S generated_cases.inc
	$(CC) $(CFLAGS) source/main.c source/runner.S -o $@ $(LDFLAGS) $(LIBS)

oracle.dol: oracle.elf
	$(ELF2DOL) $< $@

clean:
	rm -f *.elf *.dol *.map generate_cases generated_cases.inc coverage.json \
		host_diff host_diff_gen host_fixture.inc host_generated.c \
		dedicated_diff dedicated_gen dedicated_generated.c \
		oracle_capture*.txt oracle_capture*.raw oracle_capture*.json

capture-interpreter: oracle.dol
	python3 run_matrix.py --out oracle_capture.interpreter.txt

capture-jit: oracle.dol
	python3 run_matrix.py --dolphin-default-cpu-core --out oracle_capture.jit.txt

verify: capture-interpreter
	python3 verify_capture.py oracle_capture.interpreter.txt --json oracle_capture.json

verify-jit: capture-interpreter capture-jit
	python3 verify_capture.py oracle_capture.interpreter.txt \
		--compare oracle_capture.jit.txt --json oracle_capture.json

host_fixture.inc: oracle_capture.interpreter.txt generate_host_fixture.py verify_capture.py
	python3 generate_host_fixture.py $< $@

host_diff_gen: host_diff_gen.c host_diff.h host_fixture.inc
	$(HOST_CC) -std=c11 -Wall -Wextra -I../../src -I. \
		host_diff_gen.c ../../src/frontend/decoder.c \
		../../src/backend/emitter.c -o $@

host_generated.c: host_diff_gen
	./host_diff_gen $@

host_diff: host_diff_run.c host_diff.h host_fixture.inc host_generated.c
	$(HOST_CC) -std=c11 -Wall -Wextra -ffp-contract=off -fno-fast-math \
		-I../../src -I. host_diff_run.c host_generated.c \
		../../src/core/cpu.c -lm -o $@

diff: host_diff
	./host_diff

dedicated_gen: dedicated_gen.c dedicated_cases.h
	$(HOST_CC) -std=c11 -Wall -Wextra -I../../src -I. \
		dedicated_gen.c ../../src/frontend/decoder.c \
		../../src/backend/emitter.c -o $@

dedicated_generated.c: dedicated_gen
	./dedicated_gen $@

dedicated_diff: dedicated_run.c dedicated_cases.h dedicated_generated.c
	$(HOST_CC) -std=c11 -Wall -Wextra -ffp-contract=off -fno-fast-math \
		-I../../src -I. dedicated_run.c dedicated_generated.c \
		../../src/core/cpu.c -lm -o $@

dedicated: dedicated_diff
	./dedicated_diff

.PHONY: all clean capture-interpreter capture-jit verify verify-jit diff dedicated
