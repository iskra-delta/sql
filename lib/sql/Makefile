cc ?= gcc
ar ?= ar
ranlib ?= ranlib
sdcc ?= sdcc
include_dir ?= ../../include
build_dir ?= ../../build

sources := sql.c
objects := $(patsubst %.c,$(build_dir)/libsql_%.o,$(sources))
depends := $(objects:.o=.d)
headers := $(wildcard $(include_dir)/*.h)
archive := $(build_dir)/libsql.a
sdcc_rel := $(build_dir)/libsql_sql.rel

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

$(build_dir)/libsql_%.o: %.c $(headers) | $(build_dir)
	$(cc) $(cflags) -MMD -MP -c $< -o $@

$(build_dir):
	mkdir -p $(build_dir)

sdcc_check: $(sources) $(headers) | $(build_dir)
	$(sdcc) --std-c11 -I$(include_dir) -c sql.c -o $(sdcc_rel)

clean:
	rm -f $(build_dir)/libsql_sql.o $(build_dir)/libsql_sql.d
	rm -f $(archive) $(sdcc_rel)

test:

-include $(depends)
