/*
 * Declares the platform I/O interface for the SQL shell.
 * All terminal input and output go through these four functions so
 * the shell body stays identical between hosted and CP/M builds.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#ifndef platform_h
#define platform_h

void platform_init(void);
void platform_exit(void);
int  read_char(void);
void write_char(char c);

#endif
