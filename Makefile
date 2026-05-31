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
	$(MAKE) -C lib/ndx debug cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
	$(MAKE) -C lib/shared debug cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
	$(MAKE) -C lib/sql debug cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
	$(MAKE) -C lib/sqlexec debug cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
	$(MAKE) -C lib/sqlopt debug cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
	$(MAKE) -C src debug cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) bin_dir=$(root_dir)/$(bin_dir) \
		include_dir=$(root_dir)/$(include_dir) \
		libdbf_dir=$(root_dir)/lib/dbf libndx_dir=$(root_dir)/lib/ndx \
		libshared_dir=$(root_dir)/lib/shared \
		libsql_dir=$(root_dir)/lib/sql \
		libsqlexec_dir=$(root_dir)/lib/sqlexec \
		libsqlopt_dir=$(root_dir)/lib/sqlopt

release:
	$(MAKE) -C lib cc=$(cc) sdcc=$(sdcc)
	$(MAKE) -C lib/dbf release cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
	$(MAKE) -C lib/ndx release cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
	$(MAKE) -C lib/shared release cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
	$(MAKE) -C lib/sql release cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
	$(MAKE) -C lib/sqlexec release cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
	$(MAKE) -C lib/sqlopt release cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
	$(MAKE) -C src release cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) bin_dir=$(root_dir)/$(bin_dir) \
		include_dir=$(root_dir)/$(include_dir) \
		libdbf_dir=$(root_dir)/lib/dbf libndx_dir=$(root_dir)/lib/ndx \
		libshared_dir=$(root_dir)/lib/shared \
		libsql_dir=$(root_dir)/lib/sql \
		libsqlexec_dir=$(root_dir)/lib/sqlexec \
		libsqlopt_dir=$(root_dir)/lib/sqlopt

run:
	$(MAKE) -C src run cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) bin_dir=$(root_dir)/$(bin_dir) \
		include_dir=$(root_dir)/$(include_dir) \
		libdbf_dir=$(root_dir)/lib/dbf libndx_dir=$(root_dir)/lib/ndx \
		libshared_dir=$(root_dir)/lib/shared \
		libsql_dir=$(root_dir)/lib/sql \
		libsqlexec_dir=$(root_dir)/lib/sqlexec \
		libsqlopt_dir=$(root_dir)/lib/sqlopt

sdcc_check:
	$(MAKE) -C lib/dbf sdcc_check cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
	$(MAKE) -C lib/ndx sdcc_check cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
	$(MAKE) -C lib/shared sdcc_check cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
	$(MAKE) -C lib/sql sdcc_check cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
	$(MAKE) -C lib/sqlexec sdcc_check cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
	$(MAKE) -C lib/sqlopt sdcc_check cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
	$(MAKE) -C src sdcc_check cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) bin_dir=$(root_dir)/$(bin_dir) \
		include_dir=$(root_dir)/$(include_dir) \
		libdbf_dir=$(root_dir)/lib/dbf libndx_dir=$(root_dir)/lib/ndx \
		libshared_dir=$(root_dir)/lib/shared \
		libsql_dir=$(root_dir)/lib/sql \
		libsqlexec_dir=$(root_dir)/lib/sqlexec \
		libsqlopt_dir=$(root_dir)/lib/sqlopt

test:
	$(MAKE) -C tests test cc=$(cc) \
		build_dir=$(root_dir)/$(build_dir) bin_dir=$(root_dir)/$(bin_dir) \
		include_dir=$(root_dir)/$(include_dir) \
		libdbf_dir=$(root_dir)/lib/dbf libndx_dir=$(root_dir)/lib/ndx \
		libshared_dir=$(root_dir)/lib/shared \
		libsql_dir=$(root_dir)/lib/sql \
		libsqlexec_dir=$(root_dir)/lib/sqlexec \
		libsqlopt_dir=$(root_dir)/lib/sqlopt

clean:
	$(MAKE) -C tests clean cc=$(cc) \
		build_dir=$(root_dir)/$(build_dir) bin_dir=$(root_dir)/$(bin_dir) \
		include_dir=$(root_dir)/$(include_dir) \
		libdbf_dir=$(root_dir)/lib/dbf libndx_dir=$(root_dir)/lib/ndx
	$(MAKE) -C src clean cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) bin_dir=$(root_dir)/$(bin_dir) \
		include_dir=$(root_dir)/$(include_dir) \
		libdbf_dir=$(root_dir)/lib/dbf libndx_dir=$(root_dir)/lib/ndx \
		libshared_dir=$(root_dir)/lib/shared \
		libsql_dir=$(root_dir)/lib/sql \
		libsqlexec_dir=$(root_dir)/lib/sqlexec \
		libsqlopt_dir=$(root_dir)/lib/sqlopt
	$(MAKE) -C lib/sqlopt clean cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
	$(MAKE) -C lib/sqlexec clean cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
	$(MAKE) -C lib/sql clean cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
	$(MAKE) -C lib/shared clean cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
	$(MAKE) -C lib/ndx clean cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
	$(MAKE) -C lib/dbf clean cc=$(cc) sdcc=$(sdcc) \
		build_dir=$(root_dir)/$(build_dir) \
		include_dir=$(root_dir)/$(include_dir)
