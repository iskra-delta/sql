/*
 * Declares the SQL execution context and the single-argument entry
 * points for the three pipeline phases (parse, optimize, execute).
 *
 * On CP/M the context lives at a fixed address in the resident kernel.
 * Each loadable module binary exports one function at offset zero:
 *
 *   int module_run(sql_context *ctx);
 *
 * The kernel writes ctx->text before loading MOD_PARSE, then calls
 * module_run; the result propagates through ctx->result.
 * The same ctx pointer is passed to MOD_OPT and the executor module
 * without copying; the sqlexec_program arena is rewritten in place.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#ifndef sqlctx_h
#define sqlctx_h

#include "sqlexec.h"

typedef struct sql_context {
    const char      *root;                    /* storage root path */
    char             current_db[sql_name_size]; /* active db; DDL may change */
    const char      *text;                    /* SQL input, set before parse */
    sqlexec_program  program;                 /* IR: written by parse, rewritten by opt */
    sqlexec_io       io;                      /* output callback */
    int              result;                  /* last phase result: 0=ok, -1=error */
} sql_context;

/*
 * Parse phase entry point.
 * Reads ctx->text and fills ctx->program.
 * Returns zero on success and -1 on parse failure.
 */
int sql_run(sql_context *ctx);

/*
 * Optimize phase entry point.
 * Rewrites ctx->program in place using catalog-driven rules.
 * Reads ctx->root and ctx->current_db for catalog access.
 * Returns zero on success and -1 on failure.
 */
int sqlopt_run(sql_context *ctx);

/*
 * Execute phase entry point.
 * Executes ctx->program against the storage at ctx->root.
 * May update ctx->current_db for database-selection statements.
 * Writes user-visible output through ctx->io.
 * Returns zero on success and -1 on failure.
 */
int sqlexec_run(sql_context *ctx);

#endif
