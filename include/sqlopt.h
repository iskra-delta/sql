/*
 * Declares a tiny optimizer for SQL execution-tree programs.
 * The optimizer inspects one sqlexec program, applies conservative
 * catalog-driven rewrites, and leaves execution to later consumers of
 * the optimized tree.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#ifndef sqlopt_h
#define sqlopt_h

#include "sqlexec.h"

/*
 * Rewrites one lowered tree using conservative catalog-driven rules.
 * The current pass checks the registered index catalog for the active
 * database and may annotate table_scan leaves with index-backed
 * equality or range access. Returns zero on success and -1 on failure.
 */
int sqlopt_optimize(sqlexec_program *program, const char *root,
    const char *db_name);

#endif
