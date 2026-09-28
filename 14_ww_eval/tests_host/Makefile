# Host unit tests for the pure-C firmware modules.
#   make -C tests_host          build and run the C tests, emit cross-check vectors
#   make -C tests_host clean
CC      ?= cc
CFLAGS  ?= -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -I../src
LIBSRC  := ../src/beamformer_delay_sum.c ../src/audio_metrics.c \
           ../src/telemetry_protocol.c ../src/eval_paths.c

all: run vectors.json

test_dsp: $(LIBSRC) ../src/*.h test_dsp.c
	$(CC) $(CFLAGS) -o $@ $(LIBSRC) test_dsp.c

gen_vectors: $(LIBSRC) ../src/*.h gen_vectors.c
	$(CC) $(CFLAGS) -o $@ $(LIBSRC) gen_vectors.c

run: test_dsp
	./test_dsp

vectors.json: gen_vectors
	./gen_vectors > vectors.json
	@cat vectors.json

clean:
	rm -rf test_dsp gen_vectors vectors.json *.dSYM

.PHONY: all run clean
