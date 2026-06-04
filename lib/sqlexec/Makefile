cc ?= gcc
ar ?= ar
ranlib ?= ranlib
sdcc ?= sdcc
sdccflags ?= --std-c11 -mz80
include_dir ?= ../../include
build_dir ?= ../../build
common_dir ?= ../common
catalog_dir ?= ../catalog

sources := sqlexec.c execute.c execute_ddl.c execute_select.c \
	execute_mutate.c execute_scan.c execute_subquery.c project.c \
	execute_sysviews.c where.c metacache.c
objects := $(patsubst %.c,$(build_dir)/libsqlexec_%.o,$(sources))
depends := $(objects:.o=.d)
headers := $(wildcard $(include_dir)/*.h) \
	$(common_dir)/common.h $(catalog_dir)/catalog.h \
	where.h metacache.h exec_impl.h
archive := $(build_dir)/libsqlexec.a
sdcc_rel := $(build_dir)/libsqlexec_sqlexec.rel \
	$(build_dir)/libsqlexec_execute.rel \
	$(build_dir)/libsqlexec_execute_ddl.rel \
	$(build_dir)/libsqlexec_execute_select.rel \
	$(build_dir)/libsqlexec_execute_mutate.rel \
	$(build_dir)/libsqlexec_execute_scan.rel \
	$(build_dir)/libsqlexec_execute_subquery.rel \
	$(build_dir)/libsqlexec_project.rel \
	$(build_dir)/libsqlexec_execute_sysviews.rel \
	$(build_dir)/libsqlexec_where.rel \
	$(build_dir)/libsqlexec_metacache.rel

cflags_common := -std=c11 -I$(include_dir) -I$(common_dir) \
	-I$(catalog_dir) \
	-Wall -Wextra -pedantic
cflags_debug := -g -fsanitize=address,undefined -DSQLEXEC_DEBUG
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

$(build_dir)/libsqlexec_%.o: %.c $(headers) | $(build_dir)
	$(cc) $(cflags) -MMD -MP -c $< -o $@

$(build_dir):
	mkdir -p $(build_dir)

sdcc_check: $(sources) $(headers) | $(build_dir)
	$(sdcc) $(sdccflags) -I$(include_dir) -I$(common_dir) -I$(catalog_dir) \
		-c sqlexec.c \
		-o $(build_dir)/libsqlexec_sqlexec.rel
	$(sdcc) $(sdccflags) -I$(include_dir) -I$(common_dir) -I$(catalog_dir) \
		-c execute.c \
		-o $(build_dir)/libsqlexec_execute.rel
	$(sdcc) $(sdccflags) -I$(include_dir) -I$(common_dir) -I$(catalog_dir) \
		-c execute_ddl.c \
		-o $(build_dir)/libsqlexec_execute_ddl.rel
	$(sdcc) $(sdccflags) -I$(include_dir) -I$(common_dir) -I$(catalog_dir) \
		-c execute_select.c \
		-o $(build_dir)/libsqlexec_execute_select.rel
	$(sdcc) $(sdccflags) -I$(include_dir) -I$(common_dir) -I$(catalog_dir) \
		-c execute_mutate.c \
		-o $(build_dir)/libsqlexec_execute_mutate.rel
	$(sdcc) $(sdccflags) -I$(include_dir) -I$(common_dir) -I$(catalog_dir) \
		-c execute_scan.c \
		-o $(build_dir)/libsqlexec_execute_scan.rel
	$(sdcc) $(sdccflags) -I$(include_dir) -I$(common_dir) -I$(catalog_dir) \
		-c execute_subquery.c \
		-o $(build_dir)/libsqlexec_execute_subquery.rel
	$(sdcc) $(sdccflags) -I$(include_dir) -I$(common_dir) -I$(catalog_dir) \
		-c project.c \
		-o $(build_dir)/libsqlexec_project.rel
	$(sdcc) $(sdccflags) -I$(include_dir) -I$(common_dir) -I$(catalog_dir) \
		-c execute_sysviews.c \
		-o $(build_dir)/libsqlexec_execute_sysviews.rel
	$(sdcc) $(sdccflags) -I$(include_dir) -I$(common_dir) -I$(catalog_dir) \
		-c where.c \
		-o $(build_dir)/libsqlexec_where.rel
	$(sdcc) $(sdccflags) -I$(include_dir) -I$(common_dir) -I$(catalog_dir) \
		-c metacache.c \
		-o $(build_dir)/libsqlexec_metacache.rel

clean:
	rm -f $(build_dir)/libsqlexec_sqlexec.o \
		$(build_dir)/libsqlexec_sqlexec.d
	rm -f $(build_dir)/libsqlexec_execute.o \
		$(build_dir)/libsqlexec_execute.d
	rm -f $(build_dir)/libsqlexec_execute_ddl.o \
		$(build_dir)/libsqlexec_execute_ddl.d
	rm -f $(build_dir)/libsqlexec_execute_select.o \
		$(build_dir)/libsqlexec_execute_select.d
	rm -f $(build_dir)/libsqlexec_execute_mutate.o \
		$(build_dir)/libsqlexec_execute_mutate.d
	rm -f $(build_dir)/libsqlexec_execute_scan.o \
		$(build_dir)/libsqlexec_execute_scan.d
	rm -f $(build_dir)/libsqlexec_execute_subquery.o \
		$(build_dir)/libsqlexec_execute_subquery.d
	rm -f $(build_dir)/libsqlexec_project.o \
		$(build_dir)/libsqlexec_project.d
	rm -f $(build_dir)/libsqlexec_execute_sysviews.o \
		$(build_dir)/libsqlexec_execute_sysviews.d
	rm -f $(build_dir)/libsqlexec_where.o \
		$(build_dir)/libsqlexec_where.d
	rm -f $(build_dir)/libsqlexec_metacache.o \
		$(build_dir)/libsqlexec_metacache.d
	rm -f $(archive) $(sdcc_rel)

test:

-include $(depends)
