cc ?= gcc
ar ?= ar
ranlib ?= ranlib
sdcc ?= sdcc
sdccflags ?= --std-c11 -mz80
include_dir ?= ../../include
build_dir ?= ../../build

sources := common.c
objects := $(patsubst %.c,$(build_dir)/libcommon_%.o,$(sources))
depends := $(objects:.o=.d)
headers := $(wildcard $(include_dir)/*.h) common.h
archive := $(build_dir)/libcommon.a
sdcc_rel := $(build_dir)/libcommon_common.rel

cflags_common := -std=c11 -I$(include_dir) -I. -Wall -Wextra -pedantic
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

$(build_dir)/libcommon_%.o: %.c $(headers) | $(build_dir)
	$(cc) $(cflags) -MMD -MP -c $< -o $@

$(build_dir):
	mkdir -p $(build_dir)

sdcc_check: $(sources) $(headers) | $(build_dir)
	$(sdcc) $(sdccflags) -I$(include_dir) -I. -c common.c \
		-o $(build_dir)/libcommon_common.rel

clean:
	rm -f $(build_dir)/libcommon_common.o \
		$(build_dir)/libcommon_common.d
	rm -f $(archive) $(sdcc_rel)

test:

-include $(depends)
