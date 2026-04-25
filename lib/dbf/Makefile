cc ?= gcc
ar ?= ar
ranlib ?= ranlib
sdcc ?= sdcc
include_dir ?= ../../include
build_dir ?= ../../build

sources := dbf.c
objects := $(patsubst %.c,$(build_dir)/libdbf_%.o,$(sources))
depends := $(objects:.o=.d)
headers := $(wildcard $(include_dir)/*.h)
archive := $(build_dir)/libdbf.a
sdcc_rel := $(build_dir)/libdbf_dbf.rel

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

$(build_dir)/libdbf_%.o: %.c $(headers) | $(build_dir)
	$(cc) $(cflags) -MMD -MP -c $< -o $@

$(build_dir):
	mkdir -p $(build_dir)

sdcc_check: $(sources) $(headers) | $(build_dir)
	$(sdcc) --std-c11 -I$(include_dir) -c dbf.c -o $(sdcc_rel)

clean:
	rm -f $(build_dir)/libdbf_dbf.o $(build_dir)/libdbf_dbf.d
	rm -f $(archive) $(sdcc_rel)

test:
