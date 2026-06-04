/*
 * Declares the in-memory transaction log types and the public API.
 * Every INSERT, UPDATE, and DELETE inside an open transaction is stored
 * as a heap-allocated txn_entry node containing the full new-record
 * payload.  No shadow files are written; all overlay lookups are a
 * linear scan of the linked list.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#ifndef tran_h
#define tran_h

#include "sqltypes.h"
#include "sqlctx.h"
#include "../common/common.h"

/* Opaque forward declaration — full definition in exec_impl.h. */
struct sqlexec_env;

/* ------------------------------------------------------------------ */
/* Transaction log types                                                */
/* ------------------------------------------------------------------ */

typedef unsigned char txn_op;
enum {
    txn_op_insert = 0,
    txn_op_update,
    txn_op_delete
};

/*
 * One DML operation held entirely in heap memory.
 *
 * INSERT : new_record holds the new row; record_no is 0 (unused).
 * UPDATE : new_record holds the replacement row; old_crc is the CRC-16
 *          of the original physical record for COMMIT precondition check.
 * DELETE : new_record is NULL; old_crc is the CRC-16 of the original.
 *
 * Freed by free_entries() on COMMIT or ROLLBACK.
 */
typedef struct txn_entry {
    txn_op             op;
    char               table[sql_name_size];
    unsigned long      record_no;
    unsigned short     old_crc;
    char              *new_record;    /* malloc'd; NULL for DELETE */
    unsigned short     record_length;
    struct txn_entry  *next;
} txn_entry;

/*
 * One SQL statement stored for potential conflict replay.
 * text is heap-allocated; freed with free_stmts().
 */
typedef struct txn_stmt {
    char            *text;
    struct txn_stmt *next;
} txn_stmt;

/* ------------------------------------------------------------------ */
/* Write-side API  (called from execute_mutate.c)                       */
/* ------------------------------------------------------------------ */

/*
 * Allocates a txn_entry and copies record into it.
 * Falls through to -1 only on allocation failure.
 * The caller must NOT call dbf_append / dbf_write / dbf_delete when
 * this returns 0.
 */
int txn_insert(struct sqlexec_env *env, const char *table_name,
    const char *record, unsigned short record_length);

int txn_update(struct sqlexec_env *env, const char *table_name,
    unsigned long record_no,
    const char *old_record, const char *new_record,
    unsigned short record_length);

int txn_delete(struct sqlexec_env *env, const char *table_name,
    unsigned long record_no,
    const char *old_record, unsigned short record_length);

/* ------------------------------------------------------------------ */
/* Read-side API  (called from execute_select.c, execute_mutate.c)     */
/* ------------------------------------------------------------------ */

/*
 * Scans the in-memory entry list for an overlay of one physical record.
 * Returns  0 — use physical record as-is.
 * Returns  1 — record was deleted; skip it.
 * Copies new_record into record[] and returns 0 when the record was
 * updated.
 * Always returns 0 immediately when no transaction is active.
 */
int txn_overlay_record(const struct sqlexec_env *env,
    const char *table_name, unsigned long record_no,
    char *record, unsigned short record_length);

typedef int (*txn_insert_cb)(unsigned long virtual_recno,
    const char *record, unsigned short record_length, void *ctx);

/*
 * Iterates all pending INSERT entries for table_name and calls cb for
 * each one.  Returns zero on success and -1 on callback error.
 */
int txn_scan_inserts(const struct sqlexec_env *env,
    const char *table_name, txn_insert_cb cb, void *ctx);

/* ------------------------------------------------------------------ */
/* Statement recording                                                  */
/* ------------------------------------------------------------------ */

void txn_record_stmt(struct sql_txn *txn, const char *text);

/* ------------------------------------------------------------------ */
/* Executor entry point  (called from execute.c)                        */
/* ------------------------------------------------------------------ */

int exec_txn(struct sqlexec_env *env);

#endif
