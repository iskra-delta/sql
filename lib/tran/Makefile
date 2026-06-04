cc ?= gcc
ar ?= ar
ranlib ?= ranlib
sdcc ?= sdcc
sdccflags ?= --std-c11 -mz80
include_dir ?= ../../include
build_dir ?= ../../build
common_dir ?= ../common
catalog_dir ?= ../catalog
sqlexec_dir ?= ../sqlexec

sources := tran.c
objects := $(patsubst %.c,$(build_dir)/libtran_%.o,$(sources))
depends := $(objects:.o=.d)
headers := $(wildcard $(include_dir)/*.h) tran.h
archive := $(build_dir)/libtran.a
sdcc_rel := $(build_dir)/libtran_tran.rel

cflags_common := -std=c11 \
    -I$(include_dir) \
    -I$(common_dir) \
    -I$(catalog_dir) \
    -I$(sqlexec_dir) \
    -I. \
    -Wall -Wextra -pedantic
cflags_debug   := -g -fsanitize=address,undefined
cflags_release := -O2

.PHONY: all debug release clean sdcc_check

all: debug

debug: cflags := $(cflags_common) $(cflags_debug)
debug: $(archive)

release: cflags := $(cflags_common) $(cflags_release)
release: $(archive)

$(archive): $(objects) | $(build_dir)
	$(ar) rcs $@ $(objects)
	$(ranlib) $@

$(build_dir)/libtran_%.o: %.c $(headers) | $(build_dir)
	$(cc) $(cflags) -MMD -MP -c $< -o $@

$(build_dir):
	mkdir -p $(build_dir)

sdcc_check: $(sources) $(headers) | $(build_dir)
	$(sdcc) $(sdccflags) \
	    -I$(include_dir) -I$(common_dir) -I$(catalog_dir) \
	    -I$(sqlexec_dir) -I. \
	    -c tran.c -o $(sdcc_rel)

clean:
	rm -f $(objects) $(depends) $(archive) $(sdcc_rel)

-include $(depends)
