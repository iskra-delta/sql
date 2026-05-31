project_name := sql

cc ?= gcc
sdcc ?= sdcc
include_dir ?= ../include
build_dir ?= ../build
bin_dir ?= ../bin
libdbf_dir ?= ../lib/dbf
libndx_dir ?= ../lib/ndx
libshared_dir ?= ../lib/shared
libsql_dir ?= ../lib/sql
libsqlexec_dir ?= ../lib/sqlexec
libsqlopt_dir ?= ../lib/sqlopt

app_sources := main.c platform.c
app_objects := $(patsubst %.c,$(build_dir)/src_%.o,$(app_sources))
objects := $(app_objects)
depends := $(objects:.o=.d)
headers := $(wildcard $(include_dir)/*.h)
libdbf_archive := $(build_dir)/libdbf.a
libndx_archive := $(build_dir)/libndx.a
libshared_archive := $(build_dir)/libshared.a
libsql_archive := $(build_dir)/libsql.a
libsqlexec_archive := $(build_dir)/libsqlexec.a
libsqlopt_archive := $(build_dir)/libsqlopt.a

cflags_common := -std=c11 -I$(include_dir) -Wall -Wextra -pedantic
cflags_debug := -g -fsanitize=address,undefined
cflags_release := -O2
ldflags_debug := -fsanitize=address,undefined

target := $(bin_dir)/$(project_name)

.PHONY: all debug release run clean sdcc_check \
	force_libdbf force_libndx force_libshared \
	force_libsql force_libsqlexec force_libsqlopt

all: debug

debug: cflags := $(cflags_common) $(cflags_debug)
debug: ldflags := $(ldflags_debug)
debug: $(target)

release: cflags := $(cflags_common) $(cflags_release)
release: ldflags :=
release: $(target)

run: debug
	$(target)

$(target): $(objects) $(libdbf_archive) $(libndx_archive) \
		$(libshared_archive) $(libsql_archive) \
		$(libsqlexec_archive) $(libsqlopt_archive) | $(bin_dir)
	$(cc) $(objects) \
		$(libsql_archive) $(libsqlexec_archive) $(libsqlopt_archive) \
		$(libshared_archive) $(libndx_archive) $(libdbf_archive) \
		$(ldflags) -o $@

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

$(libndx_archive): force_libndx
	$(MAKE) -C $(libndx_dir) debug cc=$(cc) sdcc=$(sdcc) \
		include_dir=$(abspath $(include_dir)) \
		build_dir=$(abspath $(build_dir))

$(libshared_archive): force_libshared
	$(MAKE) -C $(libshared_dir) debug cc=$(cc) sdcc=$(sdcc) \
		include_dir=$(abspath $(include_dir)) \
		build_dir=$(abspath $(build_dir))

$(libsql_archive): force_libsql
	$(MAKE) -C $(libsql_dir) debug cc=$(cc) sdcc=$(sdcc) \
		include_dir=$(abspath $(include_dir)) \
		build_dir=$(abspath $(build_dir))

$(libsqlexec_archive): force_libsqlexec
	$(MAKE) -C $(libsqlexec_dir) debug cc=$(cc) sdcc=$(sdcc) \
		include_dir=$(abspath $(include_dir)) \
		build_dir=$(abspath $(build_dir))

$(libsqlopt_archive): force_libsqlopt
	$(MAKE) -C $(libsqlopt_dir) debug cc=$(cc) sdcc=$(sdcc) \
		include_dir=$(abspath $(include_dir)) \
		build_dir=$(abspath $(build_dir))

sdcc_check: $(app_sources) $(headers) | $(build_dir)
	$(sdcc) --std-c11 -I$(include_dir) -c main.c \
		-o $(build_dir)/sdcc_main.rel
	$(sdcc) --std-c11 -I$(include_dir) -c platform.c \
		-o $(build_dir)/sdcc_platform.rel

clean:
	rm -f $(build_dir)/src_main.o $(build_dir)/src_main.d
	rm -f $(build_dir)/src_platform.o $(build_dir)/src_platform.d
	rm -f $(build_dir)/sdcc_main.rel $(build_dir)/sdcc_platform.rel
	rm -f $(target) $(bin_dir)/sql_demo

-include $(depends)
