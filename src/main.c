/*
 * Interactive SQL shell entry point.
 * Reads SQL statements line by line, then drives the three pipeline
 * phases (parse, optimize, execute) through a single sql_context.
 * On CP/M each phase will be a separately loaded overlay module;
 * here they are called directly from the same address space.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "sqlctx.h"
#include "platform.h"

#define sql_buffer_size 256

#define CTRL_C  3
#define BS      8
#define DEL     127

static void write_nl(void)
{
    write_char('\r');
    write_char('\n');
}

static void write_str(const char *s)
{
    while (*s) {
        write_char(*s++);
    }
}

static void shell(const char *root)
{
    char buf[sql_buffer_size];
    sql_context ctx;
    unsigned short len;
    int c;

    ctx.root = root;
    ctx.current_db[0] = '\0';
    ctx.io.write_char = write_char;

    while (1) {
        if (ctx.current_db[0]) {
            write_str(ctx.current_db);
            write_char('>');
        } else {
            write_char('>');
        }
        write_char(' ');
        len = 0;

        while (1) {
            c = read_char();
            if (c < 0) {
                write_nl();
                return;
            }
            if (c == CTRL_C) {
                write_str("^C");
                write_nl();
                return;
            }
            if (c == '\r' || c == '\n') {
                write_nl();
                break;
            }
            if ((c == BS || c == DEL) && len > 0) {
                len--;
                write_char(BS);
                write_char(' ');
                write_char(BS);
                continue;
            }
            if (c >= 32 && len + 1 < sql_buffer_size) {
                buf[len++] = (char)c;
                write_char((char)c);
            }
        }

        buf[len] = '\0';
        if (!len) {
            continue;
        }

        ctx.text = buf;
        if (sql_run(&ctx) != 0) {
            write_str("parse error");
            write_nl();
            continue;
        }
        if (sqlopt_run(&ctx) != 0) {
            write_str("error");
            write_nl();
            continue;
        }
        if (sqlexec_run(&ctx) != 0) {
            write_str("error");
            write_nl();
        }
    }
}

#if defined(__SDCC)
int main(void)
{
    platform_init();
    shell("db");
    platform_exit();
    return 0;
}
#else
int main(int argc, char *argv[])
{
    platform_init();
    if (argc < 2) {
        write_str("usage: sql <root>");
        write_nl();
        platform_exit();
        return 1;
    }
    shell(argv[1]);
    platform_exit();
    return 0;
}
#endif
