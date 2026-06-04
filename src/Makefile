project_name := sql

cc ?= gcc
sdcc ?= sdcc
sdccflags ?= --std-c11 -mz80
include_dir ?= ../include
build_dir ?= ../build
bin_dir ?= ../bin
libcommon_dir ?= ../lib/common
libcatalog_dir ?= ../lib/catalog
libdbf_dir ?= ../lib/dbf
libndx_dir ?= ../lib/ndx
libsql_dir ?= ../lib/sql
libsqlexec_dir ?= ../lib/sqlexec
libsqlopt_dir ?= ../lib/sqlopt
libtran_dir   ?= ../lib/tran

app_sources := main.c platform.c
app_objects := $(patsubst %.c,$(build_dir)/src_%.o,$(app_sources))
objects := $(app_objects)
depends := $(objects:.o=.d)
headers := $(wildcard $(include_dir)/*.h)
libcommon_archive := $(build_dir)/libcommon.a
libcatalog_archive := $(build_dir)/libcatalog.a
libdbf_archive := $(build_dir)/libdbf.a
libndx_archive := $(build_dir)/libndx.a
libsql_archive := $(build_dir)/libsql.a
libsqlexec_archive := $(build_dir)/libsqlexec.a
libsqlopt_archive  := $(build_dir)/libsqlopt.a
libtran_archive    := $(build_dir)/libtran.a

cflags_common := -std=c11 -I$(include_dir) -Wall -Wextra -pedantic
cflags_debug := -g -fsanitize=address,undefined
cflags_release := -O2
ldflags_debug := -fsanitize=address,undefined

target := $(bin_dir)/$(project_name)

.PHONY: all debug release run clean sdcc_check \
	force_libcommon force_libcatalog force_libdbf force_libndx \
	force_libsql force_libsqlexec force_libsqlopt force_libtran

all: debug

debug: cflags := $(cflags_common) $(cflags_debug)
debug: ldflags := $(ldflags_debug)
debug: $(target)

release: cflags := $(cflags_common) $(cflags_release)
release: ldflags :=
release: $(target)

run: debug
	$(target)

$(target): $(objects) $(libcommon_archive) $(libcatalog_archive) \
		$(libdbf_archive) $(libndx_archive) $(libsql_archive) \
		$(libsqlexec_archive) $(libsqlopt_archive) $(libtran_archive) | $(bin_dir)
	$(cc) $(objects) \
		$(libsql_archive) $(libsqlexec_archive) $(libtran_archive) \
		$(libsqlopt_archive) \
		$(libcatalog_archive) $(libcommon_archive) \
		$(libndx_archive) $(libdbf_archive) \
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

$(libcommon_archive): force_libcommon
	$(MAKE) -C $(libcommon_dir) debug cc=$(cc) sdcc=$(sdcc) \
		include_dir=$(abspath $(include_dir)) \
		build_dir=$(abspath $(build_dir))

$(libcatalog_archive): force_libcatalog
	$(MAKE) -C $(libcatalog_dir) debug cc=$(cc) sdcc=$(sdcc) \
		include_dir=$(abspath $(include_dir)) \
		build_dir=$(abspath $(build_dir)) \
		common_dir=$(abspath $(libcommon_dir))

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

$(libtran_archive): force_libtran
	$(MAKE) -C $(libtran_dir) debug cc=$(cc) sdcc=$(sdcc) \
		include_dir=$(abspath $(include_dir)) \
		build_dir=$(abspath $(build_dir)) \
		common_dir=$(abspath $(libcommon_dir)) \
		catalog_dir=$(abspath $(libcatalog_dir)) \
		sqlexec_dir=$(abspath $(libsqlexec_dir))

sdcc_check: $(app_sources) $(headers) | $(build_dir)
	$(sdcc) $(sdccflags) -I$(include_dir) -c main.c \
		-o $(build_dir)/sdcc_main.rel
	$(sdcc) $(sdccflags) -I$(include_dir) -c platform.c \
		-o $(build_dir)/sdcc_platform.rel

clean:
	rm -f $(build_dir)/src_main.o $(build_dir)/src_main.d
	rm -f $(build_dir)/src_platform.o $(build_dir)/src_platform.d
	rm -f $(build_dir)/sdcc_main.rel $(build_dir)/sdcc_platform.rel
	rm -f $(target) $(bin_dir)/sql_demo

-include $(depends)
