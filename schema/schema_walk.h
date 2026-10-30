/* The asn1-schema/1 walk (RFC 0024 C1), shared by asn1-viz-schema
 * (schema/walk.c) and asn1-viz-compile-trace (compile/trace.c), so that the
 * two tools give every assignment the same id.
 *
 * Ids are assignment order: modules in the order they were parsed, members
 * in source order. Modules asn1c imports from skeletons/standard-modules
 * (MT_STANDARD_MODULE) are left out; asn1c appends them after the inputs,
 * so leaving them out changes no input assignment's id. */
#ifndef ASN1VIZ_SCHEMA_WALK_H
#define ASN1VIZ_SCHEMA_WALK_H

#include <stdio.h>

#include <asn1parser.h>

/* Walks a fixed tree: assignments, references (resolved as the compiler
 * resolves them) and recursive groups. Returns the number of assignments. */
int asn1viz_schema_walk(asn1p_t *asn);

int asn1viz_schema_types(void);
asn1p_expr_t *asn1viz_schema_expr(int id);
int asn1viz_schema_modules(void);

/* Writes asn1-schema/1. code_bytes is NULL (every codeBytes is null) or one
 * value per assignment, where a negative value is written as null. */
void asn1viz_schema_write(FILE *out, const char *asn1c_commit, int fixer_result, int fixer_warnings,
                          const long *code_bytes);

/* The one-line summary asn1-viz-schema prints (and its tests match). */
void asn1viz_schema_summary(FILE *out, const char *prog);

/* A JSON string literal. */
void asn1viz_json_string(FILE *out, const char *s);

#endif
