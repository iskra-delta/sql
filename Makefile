cc := gcc
sdcc := sdcc
build_dir := build
bin_dir := bin
include_dir := include
root_dir := $(CURDIR)

.PHONY: all debug release run clean sdcc_check test

all: debug

debug:
	$(MAKE) -C lib cc=$(cc) sdcc=$(sdcc)
	$(MAKE) -C lib/dbf debug cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
	$(MAKE) -C lib/sql debug cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
	$(MAKE) -C src debug cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) bin_dir=$(root_dir)/$(bin_dir) \
		include_dir=$(root_dir)/$(include_dir) \
		libdbf_dir=$(root_dir)/lib/dbf libsql_dir=$(root_dir)/lib/sql

release:
	$(MAKE) -C lib cc=$(cc) sdcc=$(sdcc)
	$(MAKE) -C lib/dbf release cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
	$(MAKE) -C lib/sql release cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
	$(MAKE) -C src release cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) bin_dir=$(root_dir)/$(bin_dir) \
		include_dir=$(root_dir)/$(include_dir) \
		libdbf_dir=$(root_dir)/lib/dbf libsql_dir=$(root_dir)/lib/sql

run:
	$(MAKE) -C src run cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) bin_dir=$(root_dir)/$(bin_dir) \
		include_dir=$(root_dir)/$(include_dir) \
		libdbf_dir=$(root_dir)/lib/dbf libsql_dir=$(root_dir)/lib/sql

sdcc_check:
	$(MAKE) -C lib/dbf sdcc_check cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
	$(MAKE) -C lib/sql sdcc_check cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
	$(MAKE) -C src sdcc_check cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) bin_dir=$(root_dir)/$(bin_dir) \
		include_dir=$(root_dir)/$(include_dir) \
		libdbf_dir=$(root_dir)/lib/dbf libsql_dir=$(root_dir)/lib/sql

test:
	$(MAKE) -C tests test cc=$(cc) \
		build_dir=$(root_dir)/$(build_dir) bin_dir=$(root_dir)/$(bin_dir) \
		include_dir=$(root_dir)/$(include_dir) \
		libdbf_dir=$(root_dir)/lib/dbf libsql_dir=$(root_dir)/lib/sql

clean:
	$(MAKE) -C tests clean cc=$(cc) \
		build_dir=$(root_dir)/$(build_dir) bin_dir=$(root_dir)/$(bin_dir) \
		include_dir=$(root_dir)/$(include_dir) \
		libdbf_dir=$(root_dir)/lib/dbf
	$(MAKE) -C src clean cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) bin_dir=$(root_dir)/$(bin_dir) \
		include_dir=$(root_dir)/$(include_dir) \
		libdbf_dir=$(root_dir)/lib/dbf libsql_dir=$(root_dir)/lib/sql
	$(MAKE) -C lib/dbf clean cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
	$(MAKE) -C lib/sql clean cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
