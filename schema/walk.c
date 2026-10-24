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

#ifndef ASN1VIZ_ASN1C_COMMIT
#define ASN1VIZ_ASN1C_COMMIT "unknown"
#endif

static int g_warnings;
static void logger(int severity, const char *fmt, ...) {
    (void)fmt;
    if(severity > 0) g_warnings++;
}

typedef struct {
    asn1p_expr_t *expr;
    int module;
    const char *kind;
    int members;
    int scc;
} type_t;

typedef struct {
    int from, to;
    const char *member;
} ref_t;

static type_t *T;
static int nT;
static ref_t *E;
static int nE, cE;

static const char *base_name(const char *p) {
    const char *s = p ? strrchr(p, '/') : NULL;
    return s ? s + 1 : (p ? p : "");
}

static const char *kind_of(const asn1p_expr_t *e) {
    switch(e->meta_type) {
    case AMT_TYPEREF: return "reference";
    case AMT_VALUE: return "value";
    case AMT_VALUESET: return "value set";
    case AMT_OBJECT: return "object";
    case AMT_OBJECTCLASS: return "class";
    case AMT_OBJECTFIELD: return "object field";
    default: break;
    }
    if(e->expr_type == A1TC_CLASSDEF) return "class";
    if(e->expr_type == A1TC_REFERENCE) return "reference";
    if((int)e->expr_type >= 0 && (size_t)e->expr_type < sizeof(asn1p_expr_type2str) / sizeof(asn1p_expr_type2str[0])
       && asn1p_expr_type2str[e->expr_type])
        return asn1p_expr_type2str[e->expr_type];
    return "other";
}

static int count_members(const asn1p_expr_t *e) {
    int n = 0;
    asn1p_expr_t *m;
    TQ_FOR(m, &e->members, next) {
        if(m->expr_type == A1TC_EXTENSIBLE) continue;
        n += 1 + count_members(m);
    }
    return n;
}

static int find_type(const asn1p_module_t *mod, const char *name) {
    for(int i = 0; i < nT; i++)
        if(T[i].expr->module == mod && T[i].expr->Identifier && !strcmp(T[i].expr->Identifier, name)) return i;
    for(int i = 0; i < nT; i++) /* parameterised forks may carry no module */
        if(T[i].expr->Identifier && !strcmp(T[i].expr->Identifier, name)) return i;
    return -1;
}

static void add_ref(int from, int to, const char *member) {
    for(int i = 0; i < nE; i++)
        if(E[i].from == from && E[i].to == to) return;
    if(nE == cE) {
        cE = cE ? cE * 2 : 256;
        E = realloc(E, (size_t)cE * sizeof *E);
    }
    E[nE].from = from;
    E[nE].to = to;
    E[nE].member = member;
    nE++;
}

static void walk_refs(asn1p_t *asn, asn1p_module_t *mod, int from, asn1p_expr_t *e, const char *member) {
    if(e->reference && e->reference->comp_count >= 1
       && (e->meta_type == AMT_TYPEREF || e->expr_type == A1TC_REFERENCE)) {
        asn1p_expr_t *target = WITH_MODULE_NAMESPACE(mod, ns, asn1f_lookup_symbol_ex(asn, ns, e, e->reference));
        if(target && target->Identifier) {
            int to = find_type(target->module ? target->module : mod, target->Identifier);
            if(to >= 0) add_ref(from, to, member);
        }
    }
    asn1p_expr_t *m;
    TQ_FOR(m, &e->members, next) walk_refs(asn, mod, from, m, m->Identifier ? m->Identifier : member);
}

/* Tarjan's SCC over the references between types. */
static int *idx, *low, *onstack, *stk, sp, counter, nscc;
static int **sccs, *scc_len;

static void strong(int v) {
    idx[v] = low[v] = counter++;
    stk[sp++] = v;
    onstack[v] = 1;
    for(int i = 0; i < nE; i++) {
        if(E[i].from != v) continue;
        int w = E[i].to;
        if(idx[w] < 0) {
            strong(w);
            if(low[w] < low[v]) low[v] = low[w];
        } else if(onstack[w] && idx[w] < low[v]) {
            low[v] = idx[w];
        }
    }
    if(low[v] == idx[v]) {
        int start = sp;
        do start--; while(stk[start] != v);
        int len = sp - start;
        int self = 0;
        for(int i = 0; i < nE; i++)
            if(E[i].from == v && E[i].to == v) self = 1;
        if(len > 1 || self) {
            sccs = realloc(sccs, (size_t)(nscc + 1) * sizeof *sccs);
            scc_len = realloc(scc_len, (size_t)(nscc + 1) * sizeof *scc_len);
            sccs[nscc] = malloc((size_t)len * sizeof(int));
            scc_len[nscc] = len;
            /* In type order, for a stable output. */
            int k = 0;
            for(int t = 0; t < nT; t++)
                for(int j = start; j < sp; j++)
                    if(stk[j] == t) sccs[nscc][k++] = t;
            for(int j = 0; j < len; j++) T[sccs[nscc][j]].scc = nscc;
            nscc++;
        }
        for(int j = start; j < sp; j++) onstack[stk[j]] = 0;
        sp = start;
    }
}

static void json_string(FILE *out, const char *s) {
    fputc('"', out);
    for(; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if(c == '"' || c == '\\') fprintf(out, "\\%c", c);
        else if(c < 0x20) fprintf(out, "\\u%04x", c);
        else fputc(c, out);
    }
    fputc('"', out);
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

    /* Modules and assignments. */
    int nmods = 0;
    asn1p_module_t *mod;
    TQ_FOR(mod, &asn->modules, mod_next) {
        asn1p_expr_t *e;
        TQ_FOR(e, &mod->members, next) {
            T = realloc(T, (size_t)(nT + 1) * sizeof *T);
            T[nT].expr = e;
            T[nT].module = nmods;
            T[nT].kind = kind_of(e);
            T[nT].members = count_members(e);
            T[nT].scc = -1;
            if(!e->module) e->module = mod;
            nT++;
        }
        nmods++;
    }
    /* References, resolved as the compiler does. */
    int t = 0;
    TQ_FOR(mod, &asn->modules, mod_next) {
        asn1p_expr_t *e;
        TQ_FOR(e, &mod->members, next) {
            walk_refs(asn, mod, t, e, NULL);
            t++;
        }
    }
    /* Recursion. */
    idx = malloc((size_t)(nT + 1) * sizeof(int));
    low = malloc((size_t)(nT + 1) * sizeof(int));
    onstack = calloc((size_t)nT + 1, sizeof(int));
    stk = malloc((size_t)(nT + 1) * sizeof(int));
    for(int i = 0; i < nT; i++) idx[i] = -1;
    for(int i = 0; i < nT; i++)
        if(idx[i] < 0) strong(i);

    FILE *out = out_path ? fopen(out_path, "wb") : stdout;
    if(!out) {
        fprintf(stderr, "asn1-viz-schema: cannot write %s\n", out_path);
        return 1;
    }
    fprintf(out, "{\n  \"format\": \"asn1-schema/1\",\n");
    fprintf(out, "  \"asn1c\": { \"repo\": \"dmccoystephenson/asn1c\", \"commit\": \"%s\" },\n", ASN1VIZ_ASN1C_COMMIT);
    fprintf(out, "  \"fixer\": { \"result\": %d, \"warnings\": %d },\n", fx, g_warnings);
    fprintf(out, "  \"modules\": [\n");
    int m = 0;
    TQ_FOR(mod, &asn->modules, mod_next) {
        fprintf(out, "    { \"name\": ");
        json_string(out, mod->ModuleName);
        fprintf(out, ", \"file\": ");
        json_string(out, base_name(mod->source_file_name));
        fprintf(out, ", \"imports\": [");
        asn1p_xports_t *x;
        int k = 0;
        TQ_FOR(x, &mod->imports, xp_next) {
            if(k++) fputs(", ", out);
            json_string(out, x->fromModuleName);
        }
        fprintf(out, "] }%s\n", ++m < nmods ? "," : "");
    }
    fprintf(out, "  ],\n  \"types\": [\n");
    for(int i = 0; i < nT; i++) {
        const asn1p_expr_t *e = T[i].expr;
        fprintf(out, "    { \"id\": %d, \"module\": %d, \"name\": ", i, T[i].module);
        json_string(out, e->Identifier);
        fprintf(out, ", \"kind\": ");
        json_string(out, T[i].kind);
        fprintf(out, ", \"members\": %d, \"line\": %d, \"codeBytes\": null, \"scc\": %d }%s\n", T[i].members,
                e->_lineno, T[i].scc, i + 1 < nT ? "," : "");
    }
    fprintf(out, "  ],\n  \"refs\": [\n");
    for(int i = 0; i < nE; i++) {
        fprintf(out, "    { \"from\": %d, \"to\": %d", E[i].from, E[i].to);
        if(E[i].member) {
            fprintf(out, ", \"member\": ");
            json_string(out, E[i].member);
        }
        fprintf(out, " }%s\n", i + 1 < nE ? "," : "");
    }
    fprintf(out, "  ],\n  \"sccs\": [");
    for(int i = 0; i < nscc; i++) {
        fprintf(out, "%s[", i ? ", " : "");
        for(int j = 0; j < scc_len[i]; j++) fprintf(out, "%s%d", j ? ", " : "", sccs[i][j]);
        fputc(']', out);
    }
    fprintf(out, "]\n}\n");
    if(out != stdout) fclose(out);

    int types = 0, cross = 0;
    for(int i = 0; i < nT; i++)
        if(T[i].expr->meta_type == AMT_TYPE || T[i].expr->meta_type == AMT_TYPEREF) types++;
    for(int i = 0; i < nE; i++)
        if(T[E[i].from].module != T[E[i].to].module) cross++;
    fprintf(stderr, "asn1-viz-schema: %d modules, %d assignments (%d types), %d references (%d across modules), %d recursive groups\n",
            nmods, nT, types, nE, cross, nscc);
    return 0;
}
