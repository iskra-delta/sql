/*
 * Implements platform I/O for the SQL shell.
 * The CP/M build uses BDOS calls directly; the hosted build uses
 * stdio with the terminal placed in raw mode.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "platform.h"

#if defined(__SDCC)

void platform_init(void) {}
void platform_exit(void) {}

/*
 * BDOS call 8: console input without echo.
 * Returns the character in HL (char value in L).
 */
int read_char(void)
{
    __asm
        ld   c, #8
        call 5
        ld   l, a
        ld   h, #0
    __endasm;
}

/*
 * BDOS call 2: console output.
 * First argument (char c) is at 4(ix) after the SDCC Z80 prologue.
 */
void write_char(char c)
{
    c;
    __asm
        ld   e, 4 (ix)
        ld   c, #2
        call 5
    __endasm;
}

#else

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <termios.h>
#include <unistd.h>

static struct termios saved_termios;
static int raw_active;

/*
 * Puts the terminal into raw mode so characters arrive one at a time
 * without echo and without special handling of Ctrl+C.
 * Does nothing when stdin is not a TTY.
 */
void platform_init(void)
{
    struct termios raw;
    raw_active = 0;
    if (!isatty(STDIN_FILENO))
        return;
    tcgetattr(STDIN_FILENO, &saved_termios);
    raw = saved_termios;
    cfmakeraw(&raw);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    raw_active = 1;
}

void platform_exit(void)
{
    if (raw_active)
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
}

int read_char(void)
{
    return getchar();
}

/*
 * Outputs one character.  In raw mode the kernel does not translate
 * '\n' to CR+LF, so callers must emit '\r' before '\n' explicitly.
 */
void write_char(char c)
{
    putchar((unsigned char)c);
    fflush(stdout);
}

#endif
