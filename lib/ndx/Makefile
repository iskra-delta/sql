cc ?= gcc
ar ?= ar
ranlib ?= ranlib
sdcc ?= sdcc
sdccflags ?= --std-c11 -mz80
include_dir ?= ../../include
build_dir ?= ../../build

sources := ndx.c binary64.c
objects := $(patsubst %.c,$(build_dir)/libndx_%.o,$(sources))
depends := $(objects:.o=.d)
headers := $(wildcard $(include_dir)/*.h)
archive := $(build_dir)/libndx.a
sdcc_rel := $(build_dir)/libndx_ndx.rel \
	$(build_dir)/libndx_binary64.rel

cflags_common := -std=c11 -I$(include_dir) -Wall -Wextra -pedantic
cflags_debug := -g -fsanitize=address,undefined
cflags_release := -O2

.PHONY: all debug release clean sdcc_check test

all: debug

debug: cflags := $(cflags_common) $(cflags_debug)
debug: $(archive)

release: cflags := $(cflags_common) $(cflags_release)
release: $(archive)

$(archive): $(objects) | $(build_dir)
	$(ar) rcs $@ $(objects)
	$(ranlib) $@

$(build_dir)/libndx_%.o: %.c $(headers) | $(build_dir)
	$(cc) $(cflags) -MMD -MP -c $< -o $@

$(build_dir):
	mkdir -p $(build_dir)

sdcc_check: $(sources) $(headers) | $(build_dir)
	$(sdcc) $(sdccflags) -I$(include_dir) -c ndx.c \
		-o $(build_dir)/libndx_ndx.rel
	$(sdcc) $(sdccflags) -I$(include_dir) -c binary64.c \
		-o $(build_dir)/libndx_binary64.rel

clean:
	rm -f $(build_dir)/libndx_ndx.o $(build_dir)/libndx_ndx.d
	rm -f $(build_dir)/libndx_binary64.o \
		$(build_dir)/libndx_binary64.d
	rm -f $(archive) $(sdcc_rel)

test:
