cc ?= gcc
include_dir ?= ../include
build_dir ?= ../build
bin_dir ?= ../bin
src_dir ?= ../src
libcommon_dir ?= ../lib/common
libcatalog_dir ?= ../lib/catalog
libdbf_dir ?= ../lib/dbf
libndx_dir ?= ../lib/ndx
libsql_dir ?= ../lib/sql
libsqlexec_dir ?= ../lib/sqlexec
libsqlopt_dir ?= ../lib/sqlopt

test_sources := test_dbf.c test_ndx.c test_sql.c test_sqlexec.c \
	test_sqlopt.c test_sql_exec.c
test_objects := $(patsubst %.c,$(build_dir)/test_%.o,$(test_sources))
depends := $(test_objects:.o=.d)
headers := $(wildcard $(include_dir)/*.h)
libcommon_archive := $(build_dir)/libcommon.a
libcatalog_archive := $(build_dir)/libcatalog.a
libdbf_archive := $(build_dir)/libdbf.a
libndx_archive := $(build_dir)/libndx.a
libsql_archive := $(build_dir)/libsql.a
libsqlexec_archive := $(build_dir)/libsqlexec.a
libsqlopt_archive := $(build_dir)/libsqlopt.a

cflags := -std=c11 -I$(include_dir) -Wall -Wextra -pedantic \
	-g -fsanitize=address,undefined -DSQLEXEC_DEBUG
ldflags := -fsanitize=address,undefined

dbf_target := $(bin_dir)/test_dbf
ndx_target := $(bin_dir)/test_ndx
sql_target := $(bin_dir)/test_sql
sqlexec_target := $(bin_dir)/test_sqlexec
sqlopt_target := $(bin_dir)/test_sqlopt
sql_exec_target := $(bin_dir)/test_sql_exec
sql_app := $(bin_dir)/sql

.PHONY: all test clean force_libcommon force_libcatalog force_libdbf \
	force_libndx force_libsql force_libsqlexec force_libsqlopt \
	force_sql_app

all: test

test: $(dbf_target) $(ndx_target) $(sql_target) $(sqlexec_target) \
		$(sqlopt_target) $(sql_exec_target)
	$(dbf_target)
	$(ndx_target)
	$(sql_target)
	$(sqlexec_target)
	$(sqlopt_target)
	$(sql_exec_target)

$(dbf_target): $(build_dir)/test_test_dbf.o $(libdbf_archive) | $(bin_dir)
	$(cc) $(build_dir)/test_test_dbf.o $(libdbf_archive) \
		$(ldflags) -o $@

$(ndx_target): $(build_dir)/test_test_ndx.o $(libndx_archive) \
		$(libdbf_archive) | $(bin_dir)
	$(cc) $(build_dir)/test_test_ndx.o $(libndx_archive) \
		$(libdbf_archive) $(ldflags) -o $@

$(sql_target): $(build_dir)/test_test_sql.o $(libsql_archive) \
		$(libcommon_archive) $(libcatalog_archive) \
		$(libsqlexec_archive) \
		$(libdbf_archive) | $(bin_dir)
	$(cc) $(build_dir)/test_test_sql.o \
		$(libsql_archive) $(libsqlexec_archive) \
		$(libcatalog_archive) $(libcommon_archive) \
		$(libdbf_archive) \
		$(ldflags) -o $@

$(sqlexec_target): $(build_dir)/test_test_sqlexec.o \
		$(libsqlexec_archive) $(libsql_archive) \
		$(libsqlopt_archive) $(libcommon_archive) $(libcatalog_archive) \
		$(libndx_archive) $(libdbf_archive) | $(bin_dir)
	$(cc) $(build_dir)/test_test_sqlexec.o \
		$(libsqlexec_archive) $(libsql_archive) \
		$(libsqlopt_archive) $(libcatalog_archive) $(libcommon_archive) \
		$(libndx_archive) $(libdbf_archive) \
		$(ldflags) -o $@

$(sqlopt_target): $(build_dir)/test_test_sqlopt.o \
		$(libsqlopt_archive) $(libsqlexec_archive) \
		$(libsql_archive) $(libcommon_archive) $(libcatalog_archive) \
		$(libndx_archive) $(libdbf_archive) | $(bin_dir)
	$(cc) $(build_dir)/test_test_sqlopt.o \
		$(libsqlopt_archive) $(libsqlexec_archive) \
		$(libsql_archive) $(libcatalog_archive) $(libcommon_archive) \
		$(libndx_archive) $(libdbf_archive) \
		$(ldflags) -o $@

$(sql_exec_target): $(build_dir)/test_test_sql_exec.o \
		$(libdbf_archive) $(libndx_archive) $(sql_app) | $(bin_dir)
	$(cc) $(build_dir)/test_test_sql_exec.o \
		$(libdbf_archive) $(libndx_archive) \
		$(ldflags) -o $@

$(build_dir)/test_%.o: %.c $(headers) | $(build_dir)
	$(cc) $(cflags) -MMD -MP -c $< -o $@

$(bin_dir):
	mkdir -p $(bin_dir)

$(build_dir):
	mkdir -p $(build_dir)

$(libdbf_archive): force_libdbf
	$(MAKE) -C $(libdbf_dir) debug cc=$(cc) \
		include_dir=$(abspath $(include_dir)) \
		build_dir=$(abspath $(build_dir))

$(libndx_archive): force_libndx
	$(MAKE) -C $(libndx_dir) debug cc=$(cc) \
		include_dir=$(abspath $(include_dir)) \
		build_dir=$(abspath $(build_dir))

$(libcommon_archive): force_libcommon
	$(MAKE) -C $(libcommon_dir) debug cc=$(cc) \
		include_dir=$(abspath $(include_dir)) \
		build_dir=$(abspath $(build_dir))

$(libcatalog_archive): force_libcatalog
	$(MAKE) -C $(libcatalog_dir) debug cc=$(cc) \
		include_dir=$(abspath $(include_dir)) \
		build_dir=$(abspath $(build_dir)) \
		common_dir=$(abspath $(libcommon_dir))

$(libsql_archive): force_libsql
	$(MAKE) -C $(libsql_dir) debug cc=$(cc) \
		include_dir=$(abspath $(include_dir)) \
		build_dir=$(abspath $(build_dir))

$(libsqlexec_archive): force_libsqlexec
	$(MAKE) -C $(libsqlexec_dir) debug cc=$(cc) \
		include_dir=$(abspath $(include_dir)) \
		build_dir=$(abspath $(build_dir))

$(libsqlopt_archive): force_libsqlopt
	$(MAKE) -C $(libsqlopt_dir) debug cc=$(cc) \
		include_dir=$(abspath $(include_dir)) \
		build_dir=$(abspath $(build_dir))

$(sql_app): force_sql_app
	$(MAKE) -C $(src_dir) debug cc=$(cc) \
		include_dir=$(abspath $(include_dir)) \
		build_dir=$(abspath $(build_dir)) \
		bin_dir=$(abspath $(bin_dir)) \
		libdbf_dir=$(abspath $(libdbf_dir)) \
		libndx_dir=$(abspath $(libndx_dir)) \
		libcommon_dir=$(abspath $(libcommon_dir)) \
		libcatalog_dir=$(abspath $(libcatalog_dir)) \
		libsql_dir=$(abspath $(libsql_dir)) \
		libsqlexec_dir=$(abspath $(libsqlexec_dir)) \
		libsqlopt_dir=$(abspath $(libsqlopt_dir))

clean:
	rm -f $(build_dir)/test_test_dbf.o $(build_dir)/test_test_dbf.d
	rm -f $(build_dir)/test_test_ndx.o $(build_dir)/test_test_ndx.d
	rm -f $(build_dir)/test_test_sql.o $(build_dir)/test_test_sql.d
	rm -f $(build_dir)/test_test_sqlexec.o \
		$(build_dir)/test_test_sqlexec.d
	rm -f $(build_dir)/test_test_sqlopt.o \
		$(build_dir)/test_test_sqlopt.d
	rm -f $(build_dir)/test_test_sql_exec.o \
		$(build_dir)/test_test_sql_exec.d
	rm -f $(dbf_target) $(ndx_target) $(sql_target) $(sqlexec_target) \
		$(sqlopt_target) $(sql_exec_target) \
		$(bin_dir)/demo.dbf $(bin_dir)/test.dbf $(bin_dir)/edit.dbf \
		$(bin_dir)/fields.dbf $(bin_dir)/ndx_build.dbf \
		$(bin_dir)/ndx_decimal.dbf \
		$(bin_dir)/name.ndx $(bin_dir)/id.ndx $(bin_dir)/born.ndx \
		$(bin_dir)/city_name.ndx $(bin_dir)/city_unique.ndx \
		$(bin_dir)/decimal.ndx \
		$(bin_dir)/sql_index_commands.sql \
		$(bin_dir)/sql_index_output.txt
	rm -rf $(bin_dir)/sql_index_root
	rm -rf $(bin_dir)/sqlopt_root

-include $(depends)
