cc ?= gcc
ar ?= ar
ranlib ?= ranlib
sdcc ?= sdcc
sdccflags ?= --std-c11 -mz80
include_dir ?= ../../include
build_dir ?= ../../build
common_dir ?= ../common
catalog_dir ?= ../catalog

sources := optimize.c
objects := $(patsubst %.c,$(build_dir)/libsqlopt_%.o,$(sources))
depends := $(objects:.o=.d)
headers := $(wildcard $(include_dir)/*.h) \
	$(common_dir)/common.h $(catalog_dir)/catalog.h
archive := $(build_dir)/libsqlopt.a
sdcc_rel := $(build_dir)/libsqlopt_optimize.rel

cflags_common := -std=c11 -I$(include_dir) -I$(common_dir) \
	-I$(catalog_dir) \
	-Wall -Wextra -pedantic
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

$(build_dir)/libsqlopt_%.o: %.c $(headers) | $(build_dir)
	$(cc) $(cflags) -MMD -MP -c $< -o $@

$(build_dir):
	mkdir -p $(build_dir)

sdcc_check: $(sources) $(headers) | $(build_dir)
	$(sdcc) $(sdccflags) -I$(include_dir) -I$(common_dir) -I$(catalog_dir) \
		-c optimize.c \
		-o $(build_dir)/libsqlopt_optimize.rel

clean:
	rm -f $(build_dir)/libsqlopt_optimize.o \
		$(build_dir)/libsqlopt_optimize.d
	rm -f $(archive) $(sdcc_rel)

test:

-include $(depends)
