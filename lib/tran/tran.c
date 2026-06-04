/*
 * Implements BEGIN / COMMIT / ROLLBACK using an in-memory linked list.
 * Every DML operation inside a transaction appends one txn_entry node
 * that carries the full new-record payload.  No files are written
 * during DML — the real tables are only touched at COMMIT time.
 *
 * Read overlay (SELECT, UPDATE WHERE, DELETE WHERE) scans the in-memory
 * list: O(pending_entries) per physical row, no disk I/O.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "tran.h"
#include "../sqlexec/exec_impl.h"
#include "../catalog/catalog.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

#define MAX_TXN_TABLES 8

static char *txn_strdup(const char *s)
{
    unsigned short len = (unsigned short)strlen(s);
    char *copy = (char *)malloc((unsigned short)(len + 1));
    if (copy)
        memcpy(copy, s, (unsigned short)(len + 1));
    return copy;
}

static int append_entry(struct sql_txn *txn, txn_op op,
    const char *table, unsigned long record_no, unsigned short old_crc,
    const char *new_record, unsigned short record_length)
{
    txn_entry *e;
    txn_entry *tail;

    e = (txn_entry *)malloc(sizeof(txn_entry));
    if (!e) return -1;

    e->op            = op;
    e->record_no     = record_no;
    e->old_crc       = old_crc;
    e->record_length = record_length;
    e->new_record    = NULL;
    e->next          = NULL;
    copy_name(e->table, table);

    if (new_record && record_length > 0) {
        e->new_record = (char *)malloc(record_length);
        if (!e->new_record) { free(e); return -1; }
        memcpy(e->new_record, new_record, record_length);
    }

    if (!txn->entries) {
        txn->entries = e;
        return 0;
    }
    tail = txn->entries;
    while (tail->next) tail = tail->next;
    tail->next = e;
    return 0;
}

static void free_entries(struct sql_txn *txn)
{
    txn_entry *e = txn->entries, *next;
    while (e) {
        next = e->next;
        free(e->new_record);
        free(e);
        e = next;
    }
    txn->entries = NULL;
}

static void free_stmts(struct sql_txn *txn)
{
    txn_stmt *s = txn->stmts, *next;
    while (s) { next = s->next; free(s->text); free(s); s = next; }
    txn->stmts = NULL;
}

/* Collect distinct table names from the entry list. */
static unsigned char collect_tables(const struct sql_txn *txn,
    char tables[MAX_TXN_TABLES][sql_name_size])
{
    const txn_entry *e;
    unsigned char count = 0, i;
    int found;

    for (e = txn->entries; e && count < MAX_TXN_TABLES; e = e->next) {
        found = 0;
        for (i = 0; i < count; i++) {
            if (strcmp(tables[i], e->table) == 0) { found = 1; break; }
        }
        if (!found)
            copy_name(tables[count++], e->table);
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* Write-side API                                                       */
/* ------------------------------------------------------------------ */

int txn_insert(struct sqlexec_env *env, const char *table_name,
    const char *record, unsigned short record_length)
{
    if (!env->txn || !env->txn->active) return -1;
    return append_entry(env->txn, txn_op_insert, table_name,
        0, 0, record, record_length);
}

int txn_update(struct sqlexec_env *env, const char *table_name,
    unsigned long record_no,
    const char *old_record, const char *new_record,
    unsigned short record_length)
{
    unsigned short old_crc;
    if (!env->txn || !env->txn->active) return -1;
    old_crc = crc16(old_record, record_length);
    return append_entry(env->txn, txn_op_update, table_name,
        record_no, old_crc, new_record, record_length);
}

int txn_delete(struct sqlexec_env *env, const char *table_name,
    unsigned long record_no,
    const char *old_record, unsigned short record_length)
{
    unsigned short old_crc;
    if (!env->txn || !env->txn->active) return -1;
    old_crc = crc16(old_record, record_length);
    return append_entry(env->txn, txn_op_delete, table_name,
        record_no, old_crc, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* Read-side API                                                        */
/* ------------------------------------------------------------------ */

int txn_overlay_record(const struct sqlexec_env *env,
    const char *table_name, unsigned long record_no,
    char *record, unsigned short record_length)
{
    const txn_entry *e;

    if (!env->txn || !env->txn->active) return 0;

    for (e = env->txn->entries; e; e = e->next) {
        if (e->op == txn_op_insert) continue;
        if (e->record_no != record_no) continue;
        if (strcmp(e->table, table_name) != 0) continue;

        if (e->op == txn_op_delete)
            return 1;   /* skip — deleted in this transaction */

        if (e->op == txn_op_update && e->new_record) {
            memcpy(record, e->new_record,
                record_length < e->record_length
                    ? record_length : e->record_length);
            return 0;   /* record[] now holds the updated version */
        }
    }
    return 0;
}

int txn_scan_inserts(const struct sqlexec_env *env,
    const char *table_name, txn_insert_cb cb, void *ctx)
{
    const txn_entry *e;
    unsigned long vrecno = 0;

    if (!env->txn || !env->txn->active) return 0;

    for (e = env->txn->entries; e; e = e->next) {
        if (e->op != txn_op_insert) continue;
        if (strcmp(e->table, table_name) != 0) continue;
        if (cb(vrecno++, e->new_record, e->record_length, ctx) != 0)
            return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Statement recording                                                  */
/* ------------------------------------------------------------------ */

void txn_record_stmt(struct sql_txn *txn, const char *text)
{
    txn_stmt *s;
    txn_stmt *tail;

    if (!txn || !text || !text[0]) return;
    s = (txn_stmt *)malloc(sizeof(txn_stmt));
    if (!s) return;
    s->text = txn_strdup(text);
    if (!s->text) { free(s); return; }
    s->next = NULL;

    if (!txn->stmts) { txn->stmts = s; return; }
    tail = txn->stmts;
    while (tail->next) tail = tail->next;
    tail->next = s;
}

/* ------------------------------------------------------------------ */
/* BEGIN / ROLLBACK / COMMIT                                            */
/* ------------------------------------------------------------------ */

static int exec_begin(struct sqlexec_env *env)
{
    if (!env->txn) return -1;
    if (env->txn->active) return -1;
    env->txn->active  = 1;
    env->txn->entries = NULL;
    env->txn->stmts   = NULL;
    return 0;
}

static int exec_rollback(struct sqlexec_env *env)
{
    if (!env->txn || !env->txn->active) return -1;
    free_entries(env->txn);
    free_stmts(env->txn);
    env->txn->active = 0;
    return 0;
}

/* Phase 1: CRC precondition check — no writes. */
static int commit_check(struct sqlexec_env *env)
{
    const txn_entry *e;
    dbf_file file;
    dbf_field *fields;
    unsigned short offsets[sql_max_columns];
    char *buf;
    unsigned short live_crc;
    int ret = 0;

    for (e = env->txn->entries; e; e = e->next) {
        if (e->op == txn_op_insert) continue;

        fields = (dbf_field *)malloc(sql_max_columns * sizeof(dbf_field));
        if (!fields) return -1;
        if (open_table_file(env->root, env->current_db,
            e->table, &file, fields, offsets) != 0) {
            free(fields); return -1;
        }
        free(fields);

        buf = (char *)malloc(file.record_length);
        if (!buf) { dbf_close(&file); return -1; }

        if (dbf_read(&file, e->record_no, buf) != 0) {
            free(buf); dbf_close(&file); return -1;
        }
        live_crc = crc16(buf, file.record_length);
        free(buf);
        dbf_close(&file);

        if (live_crc != e->old_crc) { ret = -1; break; }
    }
    return ret;
}

/* Phase 2: apply every entry to the real tables. */
static int commit_apply(struct sqlexec_env *env)
{
    char tables[MAX_TXN_TABLES][sql_name_size];
    unsigned char count = collect_tables(env->txn, tables);
    unsigned char t;

    for (t = 0; t < count; t++) {
        dbf_file file;
        dbf_field *fields;
        unsigned short offsets[sql_max_columns];
        const txn_entry *e;
        int ret = 0;

        fields = (dbf_field *)malloc(sql_max_columns * sizeof(dbf_field));
        if (!fields) return -1;
        if (open_table_file(env->root, env->current_db,
            tables[t], &file, fields, offsets) != 0) {
            free(fields); return -1;
        }
        free(fields);

        /* Replay entries for this table in log order. */
        for (e = env->txn->entries; e; e = e->next) {
            if (strcmp(e->table, tables[t]) != 0) continue;
            if (e->op == txn_op_delete) {
                if (dbf_delete(&file, e->record_no) != 0) { ret = -1; break; }
            } else if (e->op == txn_op_update) {
                if (dbf_write(&file, e->record_no, e->new_record) != 0) { ret = -1; break; }
            } else if (e->op == txn_op_insert) {
                if (dbf_append(&file, e->new_record) != 0) { ret = -1; break; }
            }
        }

        dbf_close(&file);
        if (ret != 0) return -1;
        rebuild_table_indexes(env->root, env->current_db, tables[t]);
    }
    return 0;
}

static int exec_commit(struct sqlexec_env *env)
{
    if (!env->txn || !env->txn->active) return -1;

    if (commit_check(env) != 0)
        return -1;  /* conflict; caller should ROLLBACK and retry */

    if (commit_apply(env) != 0) {
        free_entries(env->txn);
        free_stmts(env->txn);
        env->txn->active = 0;
        return -1;
    }

    free_entries(env->txn);
    free_stmts(env->txn);
    env->txn->active = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                   */
/* ------------------------------------------------------------------ */

int exec_txn(struct sqlexec_env *env)
{
    const sqlexec_node *root_node;

    if (!env || !env->program) return -1;
    root_node = sqlexec_get_const(env->program, env->program->root);
    if (!root_node) return -1;

    switch (root_node->opcode) {
    case sqlexec_begin:    return exec_begin(env);
    case sqlexec_commit:   return exec_commit(env);
    case sqlexec_rollback: return exec_rollback(env);
    default:               return -1;
    }
}
