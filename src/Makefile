project_name := sql_demo

cc ?= gcc
sdcc ?= sdcc
include_dir ?= ../include
build_dir ?= ../build
bin_dir ?= ../bin
libdbf_dir ?= ../lib/dbf
libsql_dir ?= ../lib/sql

app_sources := main.c
app_objects := $(patsubst %.c,$(build_dir)/src_%.o,$(app_sources))
objects := $(app_objects)
depends := $(objects:.o=.d)
headers := $(wildcard $(include_dir)/*.h)
libdbf_archive := $(build_dir)/libdbf.a
libsql_archive := $(build_dir)/libsql.a

cflags_common := -std=c11 -I$(include_dir) -Wall -Wextra -pedantic
cflags_debug := -g -fsanitize=address,undefined
cflags_release := -O2
ldflags_debug := -fsanitize=address,undefined

target := $(bin_dir)/$(project_name)

.PHONY: all debug release run clean sdcc_check force_libdbf force_libsql

all: debug

debug: cflags := $(cflags_common) $(cflags_debug)
debug: ldflags := $(ldflags_debug)
debug: $(target)

release: cflags := $(cflags_common) $(cflags_release)
release: ldflags :=
release: $(target)

run: debug
	$(target)

$(target): $(objects) $(libdbf_archive) $(libsql_archive) | $(bin_dir)
	$(cc) $(objects) $(libdbf_archive) $(libsql_archive) $(ldflags) -o $@

$(build_dir)/src_%.o: %.c $(headers) | $(build_dir)
	$(cc) $(cflags) -MMD -MP -c $< -o $@

$(bin_dir):
	mkdir -p $(bin_dir)

$(build_dir):
	mkdir -p $(build_dir)

$(libdbf_archive): force_libdbf
	$(MAKE) -C $(libdbf_dir) debug cc=$(cc) sdcc=$(sdcc) \
		include_dir=$(abspath $(include_dir)) \
		build_dir=$(abspath $(build_dir))

$(libsql_archive): force_libsql
	$(MAKE) -C $(libsql_dir) debug cc=$(cc) sdcc=$(sdcc) \
		include_dir=$(abspath $(include_dir)) \
		build_dir=$(abspath $(build_dir))

sdcc_check: $(app_sources) $(headers) | $(build_dir)
	$(sdcc) --std-c11 -I$(include_dir) -c main.c \
		-o $(build_dir)/sdcc_main.rel

clean:
	rm -f $(build_dir)/src_main.o $(build_dir)/src_main.d
	rm -f $(build_dir)/sdcc_main.rel
	rm -f $(target)

-include $(depends)
