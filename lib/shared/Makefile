cc ?= gcc
ar ?= ar
ranlib ?= ranlib
sdcc ?= sdcc
include_dir ?= ../../include
build_dir ?= ../../build

sources := shared.c catalog.c where.c metacache.c
objects := $(patsubst %.c,$(build_dir)/libshared_%.o,$(sources))
depends := $(objects:.o=.d)
headers := $(wildcard $(include_dir)/*.h) shared.h catalog.h where.h metacache.h
archive := $(build_dir)/libshared.a
sdcc_rel := $(build_dir)/libshared_shared.rel \
	$(build_dir)/libshared_catalog.rel \
	$(build_dir)/libshared_where.rel

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

$(build_dir)/libshared_%.o: %.c $(headers) | $(build_dir)
	$(cc) $(cflags) -MMD -MP -c $< -o $@

$(build_dir):
	mkdir -p $(build_dir)

sdcc_check: $(sources) $(headers) | $(build_dir)
	$(sdcc) --std-c11 -I$(include_dir) -I. -c shared.c \
		-o $(build_dir)/libshared_shared.rel
	$(sdcc) --std-c11 -I$(include_dir) -I. -c catalog.c \
		-o $(build_dir)/libshared_catalog.rel
	$(sdcc) --std-c11 -I$(include_dir) -I. -c where.c \
		-o $(build_dir)/libshared_where.rel

clean:
	rm -f $(build_dir)/libshared_shared.o \
		$(build_dir)/libshared_shared.d
	rm -f $(build_dir)/libshared_catalog.o \
		$(build_dir)/libshared_catalog.d
	rm -f $(build_dir)/libshared_where.o \
		$(build_dir)/libshared_where.d
	rm -f $(build_dir)/libshared_metacache.o \
		$(build_dir)/libshared_metacache.d
	rm -f $(archive) $(sdcc_rel)

test:

-include $(depends)
