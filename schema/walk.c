/* asn1-viz-schema: an ASN.1 specification as asn1-schema/1 (RFC 0024 C1).
 *
 *   asn1-viz-schema [--out FILE] SCHEMA.asn1...
 *
 * Parses the files with asn1c's own libasn1parser, fixes the tree with
 * libasn1fix (asn1f_process: IMPORTS resolved, tree normalised), and walks
 * it: one entry per module (its IMPORTS), one per top-level assignment
 * (kind, nested member count, file and line), one per reference from an
 * assignment to another, resolved the way the compiler resolves them
 * (asn1f_lookup_symbol_ex in the referring module's namespace), and the
 * recursive groups (Tarjan's strongly connected components over the
 * references, with self-loops).
 *
 * The walk itself is schema_walk.c, shared with asn1-viz-compile-trace
 * (which also fills codeBytes); this tool writes codeBytes as null.
 *
 * It refuses to write anything when asn1f_process reports a fatal problem
 * (-1). It must be built against the same config.h as the libraries: the
 * parser's struct layouts depend on it. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <asn1parser.h>
#include <asn1fix.h>
#include <asn1fix_export.h>
#include <asn1_namespace.h>

#include "schema_walk.h"

#ifndef ASN1VIZ_ASN1C_COMMIT
#define ASN1VIZ_ASN1C_COMMIT "unknown"
#endif

static int g_warnings;
static void logger(int severity, const char *fmt, ...) {
    (void)fmt;
    if(severity > 0) g_warnings++;
}

int main(int argc, char **argv) {
    const char *out_path = NULL;
    asn1p_t *asn = NULL;
    int nfiles = 0;
    for(int i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "--out") && i + 1 < argc) {
            out_path = argv[++i];
            continue;
        }
        asn1p_t *a = asn1p_parse_file(argv[i], A1P_NOFLAGS);
        if(!a) {
            fprintf(stderr, "asn1-viz-schema: cannot parse %s\n", argv[i]);
            return 1;
        }
        nfiles++;
        if(!asn) {
            asn = a;
        } else {
            asn1p_module_t *mod;
            while((mod = TQ_REMOVE(&a->modules, mod_next))) TQ_ADD(&asn->modules, mod, mod_next);
        }
    }
    if(!nfiles) {
        fprintf(stderr, "usage: asn1-viz-schema [--out FILE] SCHEMA.asn1...\n");
        return 2;
    }
    int fx = asn1f_process(asn, A1F_NOFLAGS, logger);
    if(fx < 0) {
        fprintf(stderr, "asn1-viz-schema: asn1f_process reported a fatal problem; nothing written\n");
        return 1;
    }

    asn1viz_schema_walk(asn);

    FILE *out = out_path ? fopen(out_path, "wb") : stdout;
    if(!out) {
        fprintf(stderr, "asn1-viz-schema: cannot write %s\n", out_path);
        return 1;
    }
    asn1viz_schema_write(out, ASN1VIZ_ASN1C_COMMIT, fx, g_warnings, NULL);
    if(out != stdout) fclose(out);

    asn1viz_schema_summary(stderr, "asn1-viz-schema");
    return 0;
}
