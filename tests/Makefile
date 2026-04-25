cc ?= gcc
include_dir ?= ../include
build_dir ?= ../build
bin_dir ?= ../bin
libdbf_dir ?= ../lib/dbf
libsql_dir ?= ../lib/sql

test_sources := test_dbf.c test_sql.c
test_objects := $(patsubst %.c,$(build_dir)/test_%.o,$(test_sources))
depends := $(test_objects:.o=.d)
headers := $(wildcard $(include_dir)/*.h)
libdbf_archive := $(build_dir)/libdbf.a
libsql_archive := $(build_dir)/libsql.a

cflags := -std=c11 -I$(include_dir) -Wall -Wextra -pedantic \
	-g -fsanitize=address,undefined
ldflags := -fsanitize=address,undefined

dbf_target := $(bin_dir)/test_dbf
sql_target := $(bin_dir)/test_sql

.PHONY: all test clean force_libdbf force_libsql

all: test

test: $(dbf_target) $(sql_target)
	$(dbf_target)
	$(sql_target)

$(dbf_target): $(build_dir)/test_test_dbf.o $(libdbf_archive) | $(bin_dir)
	$(cc) $(build_dir)/test_test_dbf.o $(libdbf_archive) $(ldflags) -o $@

$(sql_target): $(build_dir)/test_test_sql.o $(libsql_archive) | $(bin_dir)
	$(cc) $(build_dir)/test_test_sql.o $(libsql_archive) $(ldflags) -o $@

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

$(libsql_archive): force_libsql
	$(MAKE) -C $(libsql_dir) debug cc=$(cc) \
		include_dir=$(abspath $(include_dir)) \
		build_dir=$(abspath $(build_dir))

clean:
	rm -f $(build_dir)/test_test_dbf.o $(build_dir)/test_test_dbf.d
	rm -f $(build_dir)/test_test_sql.o $(build_dir)/test_test_sql.d
	rm -f $(dbf_target) $(sql_target) $(bin_dir)/test.dbf \
		$(bin_dir)/edit.dbf

-include $(depends)
