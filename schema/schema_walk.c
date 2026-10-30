/* The asn1-schema/1 walk (RFC 0024 C1); see schema_walk.h. Moved out of
 * walk.c unchanged, except that modules imported from asn1c's
 * standard-modules are skipped and codeBytes may be given. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <asn1parser.h>
#include <asn1fix.h>
#include <asn1fix_export.h>
#include <asn1_namespace.h>

#include "schema_walk.h"

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

static asn1p_t *A;
static type_t *T;
static int nT;
static int nM;
static ref_t *E;
static int nE, cE;

static int user_module(const asn1p_module_t *mod) { return !(mod->_tags & MT_STANDARD_MODULE); }

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

void asn1viz_json_string(FILE *out, const char *s) {
    fputc('"', out);
    for(; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if(c == '"' || c == '\\') fprintf(out, "\\%c", c);
        else if(c == '\n') fputs("\\n", out);
        else if(c == '\t') fputs("\\t", out);
        else if(c < 0x20) fprintf(out, "\\u%04x", c);
        else fputc(c, out);
    }
    fputc('"', out);
}

int asn1viz_schema_walk(asn1p_t *asn) {
    A = asn;
    /* Modules and assignments. */
    asn1p_module_t *mod;
    TQ_FOR(mod, &asn->modules, mod_next) {
        if(!user_module(mod)) continue;
        asn1p_expr_t *e;
        TQ_FOR(e, &mod->members, next) {
            T = realloc(T, (size_t)(nT + 1) * sizeof *T);
            T[nT].expr = e;
            T[nT].module = nM;
            T[nT].kind = kind_of(e);
            T[nT].members = count_members(e);
            T[nT].scc = -1;
            if(!e->module) e->module = mod;
            nT++;
        }
        nM++;
    }
    /* References, resolved as the compiler does. */
    int t = 0;
    TQ_FOR(mod, &asn->modules, mod_next) {
        if(!user_module(mod)) continue;
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
    return nT;
}

int asn1viz_schema_types(void) { return nT; }
int asn1viz_schema_modules(void) { return nM; }
asn1p_expr_t *asn1viz_schema_expr(int id) { return id >= 0 && id < nT ? T[id].expr : NULL; }

void asn1viz_schema_write(FILE *out, const char *asn1c_commit, int fixer_result, int fixer_warnings,
                          const long *code_bytes) {
    fprintf(out, "{\n  \"format\": \"asn1-schema/1\",\n");
    fprintf(out, "  \"asn1c\": { \"repo\": \"dmccoystephenson/asn1c\", \"commit\": \"%s\" },\n", asn1c_commit);
    fprintf(out, "  \"fixer\": { \"result\": %d, \"warnings\": %d },\n", fixer_result, fixer_warnings);
    fprintf(out, "  \"modules\": [\n");
    int m = 0;
    asn1p_module_t *mod;
    TQ_FOR(mod, &A->modules, mod_next) {
        if(!user_module(mod)) continue;
        fprintf(out, "    { \"name\": ");
        asn1viz_json_string(out, mod->ModuleName);
        fprintf(out, ", \"file\": ");
        asn1viz_json_string(out, base_name(mod->source_file_name));
        fprintf(out, ", \"imports\": [");
        asn1p_xports_t *x;
        int k = 0;
        TQ_FOR(x, &mod->imports, xp_next) {
            if(k++) fputs(", ", out);
            asn1viz_json_string(out, x->fromModuleName);
        }
        fprintf(out, "] }%s\n", ++m < nM ? "," : "");
    }
    fprintf(out, "  ],\n  \"types\": [\n");
    for(int i = 0; i < nT; i++) {
        const asn1p_expr_t *e = T[i].expr;
        fprintf(out, "    { \"id\": %d, \"module\": %d, \"name\": ", i, T[i].module);
        asn1viz_json_string(out, e->Identifier);
        fprintf(out, ", \"kind\": ");
        asn1viz_json_string(out, T[i].kind);
        fprintf(out, ", \"members\": %d, \"line\": %d, \"codeBytes\": ", T[i].members, e->_lineno);
        if(code_bytes && code_bytes[i] >= 0) fprintf(out, "%ld", code_bytes[i]);
        else fputs("null", out);
        fprintf(out, ", \"scc\": %d }%s\n", T[i].scc, i + 1 < nT ? "," : "");
    }
    fprintf(out, "  ],\n  \"refs\": [\n");
    for(int i = 0; i < nE; i++) {
        fprintf(out, "    { \"from\": %d, \"to\": %d", E[i].from, E[i].to);
        if(E[i].member) {
            fprintf(out, ", \"member\": ");
            asn1viz_json_string(out, E[i].member);
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
}

void asn1viz_schema_summary(FILE *out, const char *prog) {
    int types = 0, cross = 0;
    for(int i = 0; i < nT; i++)
        if(T[i].expr->meta_type == AMT_TYPE || T[i].expr->meta_type == AMT_TYPEREF) types++;
    for(int i = 0; i < nE; i++)
        if(T[E[i].from].module != T[E[i].to].module) cross++;
    fprintf(out, "%s: %d modules, %d assignments (%d types), %d references (%d across modules), %d recursive groups\n",
            prog, nM, nT, types, nE, cross, nscc);
}
