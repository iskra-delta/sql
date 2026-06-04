cc ?= gcc
ar ?= ar
ranlib ?= ranlib
sdcc ?= sdcc
sdccflags ?= --std-c11 -mz80
include_dir ?= ../../include
build_dir ?= ../../build
common_dir ?= ../common

sources := catalog.c
objects := $(patsubst %.c,$(build_dir)/libcatalog_%.o,$(sources))
depends := $(objects:.o=.d)
headers := $(wildcard $(include_dir)/*.h) catalog.h \
	$(common_dir)/common.h
archive := $(build_dir)/libcatalog.a
sdcc_rel := $(build_dir)/libcatalog_catalog.rel

cflags_common := -std=c11 -I$(include_dir) -I. -I$(common_dir) \
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

$(build_dir)/libcatalog_%.o: %.c $(headers) | $(build_dir)
	$(cc) $(cflags) -MMD -MP -c $< -o $@

$(build_dir):
	mkdir -p $(build_dir)

sdcc_check: $(sources) $(headers) | $(build_dir)
	$(sdcc) $(sdccflags) -I$(include_dir) -I. -I$(common_dir) -c catalog.c \
		-o $(build_dir)/libcatalog_catalog.rel

clean:
	rm -f $(build_dir)/libcatalog_catalog.o \
		$(build_dir)/libcatalog_catalog.d
	rm -f $(archive) $(sdcc_rel)

test:

-include $(depends)
