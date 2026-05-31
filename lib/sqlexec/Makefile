cc ?= gcc
ar ?= ar
ranlib ?= ranlib
sdcc ?= sdcc
include_dir ?= ../../include
build_dir ?= ../../build
shared_dir ?= ../shared

sources := sqlexec.c execute.c execute_ddl.c execute_select.c \
	execute_mutate.c execute_scan.c execute_subquery.c \
	execute_sysviews.c
objects := $(patsubst %.c,$(build_dir)/libsqlexec_%.o,$(sources))
depends := $(objects:.o=.d)
headers := $(wildcard $(include_dir)/*.h) \
	$(shared_dir)/shared.h $(shared_dir)/catalog.h $(shared_dir)/where.h \
	exec_impl.h
archive := $(build_dir)/libsqlexec.a
sdcc_rel := $(build_dir)/libsqlexec_sqlexec.rel \
	$(build_dir)/libsqlexec_execute.rel \
	$(build_dir)/libsqlexec_execute_ddl.rel \
	$(build_dir)/libsqlexec_execute_select.rel \
	$(build_dir)/libsqlexec_execute_mutate.rel \
	$(build_dir)/libsqlexec_execute_scan.rel \
	$(build_dir)/libsqlexec_execute_subquery.rel

cflags_common := -std=c11 -I$(include_dir) -I$(shared_dir) \
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
	$(sdcc) --std-c11 -I$(include_dir) -I$(shared_dir) -c sqlexec.c \
		-o $(build_dir)/libsqlexec_sqlexec.rel
	$(sdcc) --std-c11 -I$(include_dir) -I$(shared_dir) -c execute.c \
		-o $(build_dir)/libsqlexec_execute.rel
	$(sdcc) --std-c11 -I$(include_dir) -I$(shared_dir) \
		-c execute_ddl.c \
		-o $(build_dir)/libsqlexec_execute_ddl.rel
	$(sdcc) --std-c11 -I$(include_dir) -I$(shared_dir) \
		-c execute_select.c \
		-o $(build_dir)/libsqlexec_execute_select.rel
	$(sdcc) --std-c11 -I$(include_dir) -I$(shared_dir) \
		-c execute_mutate.c \
		-o $(build_dir)/libsqlexec_execute_mutate.rel

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
	rm -f $(build_dir)/libsqlexec_execute_sysviews.o \
		$(build_dir)/libsqlexec_execute_sysviews.d
	rm -f $(archive) $(sdcc_rel)

test:

-include $(depends)
