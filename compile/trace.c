/* asn1-viz-compile-trace: one asn1c run as asn1-compile/1 (RFC 0034 K0).
 *
 *   asn1-viz-compile-trace -S SKELETONS --out TRACE.json
 *                          [--schema-out SCHEMA.json] [--schema-name NAME]
 *                          SCHEMA.asn1...
 *
 * The pinned asn1c's own libraries, run in-process, stage by stage, the way
 * the asn1c driver (asn1c/asn1c.c) runs them:
 *
 *   parse    asn1p_parse_file per input; the assignments in parse order.
 *   fix      the tree printed (libasn1print, the -E form), the standard
 *            modules imported, asn1f_process(A1F_DEBUG) with a logger that
 *            turns each debug line into an event, the tree printed again
 *            (the -E -F form) and a unified diff stored per assignment.
 *   compile  asn1_compile with asn1c's default flags plus
 *            A1C_DEBUG_OUTPUT_ORIGIN_LINES, into a temporary directory; each
 *            generated file's lines counted per emitting function (the
 *            origin comment) and attributed to its assignment by asn1c's own
 *            file naming (asn1c_make_identifier).
 *   save     the skeleton files the compiler copied, with their sizes.
 *
 * The temporary directory is deleted; only counts are kept. A stage that
 * fails is recorded as "failed" (and the stages after it as "skipped"),
 * not hidden: the trace is still written.
 *
 * With --schema-out it also writes the asn1-schema/1 walk of the same tree
 * (schema/schema_walk.c, the ids the trace uses) with codeBytes filled: the
 * bytes of each assignment's generated .c and .h without the origin
 * comments.
 *
 * Nothing in the output depends on where the build tree is: the inputs and
 * asn1c's standard modules are copied into the work directory and parsed by
 * relative paths (asn1c writes the module path into the code), and the
 * origin comments (which hold asn1c's __FILE__) are not counted in sizes.
 *
 * The fixer's debug text is not an API; it is parsed as it is at the pinned
 * asn1c.ref, and the golden tests (tests/test_compile.cpp) fail on a pin
 * bump that changes it. */
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <asn1parser.h>
#include <asn1fix.h>
#include <asn1fix_export.h>
#include <asn1print.h>
#include <asn1c_internal.h>
#include <asn1c_misc.h>

#include "schema_walk.h"

#ifndef ASN1VIZ_ASN1C_COMMIT
#define ASN1VIZ_ASN1C_COMMIT "unknown"
#endif

#define PROG "asn1-viz-compile-trace"

/* ---------------------------------------------------------------- small helpers */

typedef struct {
    char *s;
    size_t len, cap;
} buf_t;

static void buf_add(buf_t *b, const char *s, size_t n) {
    if(b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->s = realloc(b->s, b->cap);
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

static void buf_str(buf_t *b, const char *s) { buf_add(b, s, strlen(s)); }

static void buf_printf(buf_t *b, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if(n > 0) buf_add(b, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    memcpy(d, s, n);
    return d;
}

static const char *base_name(const char *p) {
    const char *s = p ? strrchr(p, '/') : NULL;
    return s ? s + 1 : (p ? p : "");
}

static char *read_stream(FILE *f, size_t *len) {
    buf_t b = {0};
    char chunk[8192];
    size_t n;
    rewind(f);
    while((n = fread(chunk, 1, sizeof chunk, f)) > 0) buf_add(&b, chunk, n);
    if(!b.s) buf_add(&b, "", 0);
    if(len) *len = b.len;
    return b.s;
}

/* Runs fn with file descriptor fd (1 or 2) going to a temporary file, and
 * returns what was written there. */
static char *capture_fd(int fd, int (*fn)(void *), void *ctx, int *rc) {
    FILE *std = fd == 1 ? stdout : stderr;
    fflush(std);
    FILE *t = tmpfile();
    if(!t) {
        *rc = fn(ctx);
        return xstrdup("");
    }
    int saved = dup(fd);
    dup2(fileno(t), fd);
    *rc = fn(ctx);
    fflush(std);
    dup2(saved, fd);
    close(saved);
    char *s = read_stream(t, NULL);
    fclose(t);
    return s;
}

/* Lines of a string, as pointers into a copy. */
typedef struct {
    char **v;
    int n;
} lines_t;

static lines_t split_lines(const char *text) {
    lines_t L = {0};
    char *s = xstrdup(text);
    int cap = 0;
    while(*s) {
        char *nl = strchr(s, '\n');
        if(nl) *nl = 0;
        if(L.n == cap) {
            cap = cap ? cap * 2 : 64;
            L.v = realloc(L.v, (size_t)cap * sizeof *L.v);
        }
        L.v[L.n++] = s;
        if(!nl) break;
        s = nl + 1;
    }
    return L;
}

/* ---------------------------------------------------------------- the tree print */

static asn1p_t *g_asn;

static int print_plain(void *ctx) {
    (void)ctx;
    return asn1print(g_asn, APF_NOFLAGS);
}
static int print_lines(void *ctx) {
    (void)ctx;
    return asn1print(g_asn, APF_LINE_COMMENTS);
}

/* A line comment "-- #line N\n" at s, after any spaces: its length, or 0. */
static size_t line_comment_at(const char *s) {
    const char *q = s;
    while(*q == ' ') q++;
    if(strncmp(q, "-- #line ", 9)) return 0;
    q += 9;
    if(*q < '0' || *q > '9') return 0;
    while(*q >= '0' && *q <= '9') q++;
    return *q == '\n' ? (size_t)(q + 1 - s) : 0;
}

/* One printed form of the tree, split per assignment: the plain -E text of
 * each top-level assignment in `assign` (user modules, source order).
 *
 * The split points are the top-level "-- #line N" comments of a second
 * print with APF_LINE_COMMENTS (nested ones can sit mid-line, after the
 * indent: "OF        -- #line 11\n Item"). Every comment is cut out of
 * that print, and the result must equal the plain print, or the split is
 * refused. */
typedef struct {
    char **text; /* per assignment; NULL for an assignment asn1print skips */
    int n;
    int lines;   /* lines of the whole plain print */
    int ok;
} printed_t;

static printed_t print_tree(asn1p_t *asn, asn1p_expr_t **assign, int nassign) {
    printed_t P = {0};
    int rc;
    g_asn = asn;
    char *plain = capture_fd(1, print_plain, NULL, &rc);
    char *commented = capture_fd(1, print_lines, NULL, &rc);
    P.lines = split_lines(plain).n;
    P.n = nassign;
    P.text = calloc((size_t)nassign + 1, sizeof *P.text);

    /* The commented print without its comments, and where each top-level
     * assignment starts in it. */
    buf_t st = {0};
    size_t *starts = NULL;
    int nstarts = 0;
    for(const char *s = commented; *s;) {
        int bol = s == commented || s[-1] == '\n';
        size_t cl = line_comment_at(s);
        if(cl) {
            if(bol && *s == '-') {
                starts = realloc(starts, (size_t)(nstarts + 1) * sizeof *starts);
                starts[nstarts++] = st.len;
            }
            s += cl;
            continue;
        }
        buf_add(&st, s, 1);
        s++;
    }
    if(!st.s) buf_add(&st, "", 0);
    if(strcmp(st.s, plain)) {
        fprintf(stderr, PROG ": the tree print does not split per assignment; no diffs\n");
        return P;
    }

    /* Which assignments asn1print prints (it skips encoding instructions). */
    int *printed_ids = malloc(((size_t)nassign + 1) * sizeof(int));
    int np = 0;
    for(int i = 0; i < nassign; i++)
        if(!(assign[i]->_mark & TM_ENCODING_INSTRUCTION)) printed_ids[np++] = i;
    if(nstarts != np) {
        fprintf(stderr, PROG ": %d printed assignments for %d assignments; no diffs\n", nstarts, np);
        return P;
    }
    for(int k = 0; k < nstarts; k++) {
        /* To the next assignment, or to the module's END line. */
        size_t from = starts[k], to = k + 1 < nstarts ? starts[k + 1] : st.len;
        const char *end = strstr(st.s + from, "\nEND\n");
        if(end && (size_t)(end - st.s) + 1 < to) to = (size_t)(end - st.s) + 1;
        while(to > from + 1 && st.s[to - 1] == '\n' && st.s[to - 2] == '\n') to--;
        P.text[printed_ids[k]] = strndup(st.s + from, to - from);
    }
    P.ok = 1;
    return P;
}

/* A unified diff of two texts (an LCS over lines), with two lines of context
 * and @@ hunk headers numbered within the assignment. Empty when equal. */
static char *unified_diff(const char *a_text, const char *b_text, int *removed, int *added) {
    lines_t A = split_lines(a_text ? a_text : ""), B = split_lines(b_text ? b_text : "");
    int n = A.n, m = B.n;
    int *L = calloc((size_t)(n + 1) * (size_t)(m + 1), sizeof(int));
#define LCS(i, j) L[(size_t)(i) * (size_t)(m + 1) + (size_t)(j)]
    for(int i = n - 1; i >= 0; i--)
        for(int j = m - 1; j >= 0; j--)
            LCS(i, j) = !strcmp(A.v[i], B.v[j]) ? LCS(i + 1, j + 1) + 1
                        : (LCS(i + 1, j) >= LCS(i, j + 1) ? LCS(i + 1, j) : LCS(i, j + 1));
    /* The edit script: ' ', '-', '+' with the line index on each side. */
    int cap = n + m + 1, ne = 0;
    char *op = malloc((size_t)cap);
    int *ai = malloc((size_t)cap * sizeof(int)), *bi = malloc((size_t)cap * sizeof(int));
    int i = 0, j = 0;
    while(i < n || j < m) {
        if(i < n && j < m && !strcmp(A.v[i], B.v[j])) {
            op[ne] = ' ', ai[ne] = i++, bi[ne] = j++;
        } else if(i < n && (j == m || LCS(i + 1, j) >= LCS(i, j + 1))) {
            op[ne] = '-', ai[ne] = i++, bi[ne] = j;
        } else {
            op[ne] = '+', ai[ne] = i, bi[ne] = j++;
        }
        ne++;
    }
#undef LCS
    free(L);
    buf_t out = {0};
    const int ctx = 2;
    for(int e = 0; e < ne;) {
        if(op[e] == ' ') {
            e++;
            continue;
        }
        /* A hunk: from ctx before this change to ctx after the last change
         * that is within 2*ctx of the next. */
        int start = e - ctx < 0 ? 0 : e - ctx;
        while(start < e && op[start] != ' ') start++;
        int last = e;
        for(int k = e; k < ne; k++) {
            if(op[k] == ' ') continue;
            if(k - last > 2 * ctx) break;
            last = k;
        }
        int stop = last + ctx + 1 > ne ? ne : last + ctx + 1;
        int a0 = ai[start], b0 = bi[start], ac = 0, bc = 0;
        for(int k = start; k < stop; k++) {
            if(op[k] != '+') ac++;
            if(op[k] != '-') bc++;
        }
        buf_printf(&out, "@@ -%d,%d +%d,%d @@\n", a0 + 1, ac, b0 + 1, bc);
        for(int k = start; k < stop; k++) {
            char c[2] = {op[k], 0};
            buf_str(&out, c);
            buf_str(&out, op[k] == '+' ? B.v[bi[k]] : A.v[ai[k]]);
            buf_str(&out, "\n");
            if(op[k] == '-') (*removed)++;
            if(op[k] == '+') (*added)++;
        }
        e = stop;
    }
    free(op);
    free(ai);
    free(bi);
    return out.s ? out.s : xstrdup("");
}

/* ---------------------------------------------------------------- the fixer trace */

typedef struct {
    char *fn;     /* the reporting function */
    char *expr;   /* the identifier the line names, or NULL */
    char *file;   /* the schema file (basename) */
    char *src;    /* asn1c source "file.c:line", or NULL */
    int line;     /* the schema line, or -1 */
    int now;      /* a "=== Now processing" line */
    int type;     /* attributed assignment, or -1 */
    char how;     /* 'p'rocessing marker, 'l'ine, 'n'ame, 'c'urrent, or 0 */
} event_t;

static event_t *EV;
static int nEV, cEV;
static int g_capturing;
static int g_fix_warnings, g_fix_fatals, g_after_fix_debug;

static int is_ident_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

static void parse_event(const char *m) {
    event_t e = {0};
    e.line = -1;
    e.type = -1;
    /* The function: "name: ..." (the LOG macro's __func__); the one line
     * asn1f_process writes itself carries no prefix. */
    const char *p = m;
    while(*p && (is_ident_char(*p) && *p != '-')) p++;
    const char *rest = m;
    if(p > m && p[0] == ':' && p[1] == ' ') {
        e.fn = strndup(m, (size_t)(p - m));
        rest = p + 2;
    } else {
        e.fn = xstrdup("asn1f_process");
    }
    /* The asn1c source: a trailing " (file.c:N)". */
    size_t len = strlen(m);
    const char *close = len && m[len - 1] == ')' ? m + len - 1 : NULL;
    const char *open = NULL;
    if(close)
        for(const char *q = close; q > m; q--)
            if(*q == '(') {
                open = q;
                break;
            }
    const char *tail = m + len;
    if(open && strchr(open, ':') && open > m && open[-1] == ' ') {
        char *src = strndup(open + 1, (size_t)(close - open - 1));
        e.src = xstrdup(base_name(src)); /* __FILE__ is a build path */
        free(src);
        tail = open - 1;
    }
    /* The schema file: " in PATH" just before the source. */
    for(const char *q = tail - 4; q >= rest; q--)
        if(!strncmp(q, " in ", 4)) {
            char *path = strndup(q + 4, (size_t)(tail - q - 4));
            e.file = xstrdup(base_name(path));
            free(path);
            tail = q;
            break;
        }
    /* The schema line: "for line N" / "at line N". */
    for(const char *q = rest; q + 6 <= tail; q++)
        if(!strncmp(q, " line ", 6) && q[6] >= '0' && q[6] <= '9') {
            e.line = atoi(q + 6);
            break;
        }
    /* The expression: "(Name)" or ("Name") right after the function, or the
     * "=== Now processing "Name"" marker. */
    if(!strncmp(rest, "=== Now processing \"", 20)) {
        const char *s = rest + 20, *q = strchr(s, '"');
        if(q) e.expr = strndup(s, (size_t)(q - s));
        e.now = 1;
    } else if(rest[0] == '(') {
        const char *s = rest + 1;
        int quoted = *s == '"';
        if(quoted) s++;
        const char *q = s;
        while(is_ident_char(*q)) q++;
        if(q > s && (quoted ? (q[0] == '"' && q[1] == ')') : q[0] == ')')) e.expr = strndup(s, (size_t)(q - s));
    }
    if(nEV == cEV) {
        cEV = cEV ? cEV * 2 : 1024;
        EV = realloc(EV, (size_t)cEV * sizeof *EV);
    }
    EV[nEV++] = e;
}

static void fixer_logger(int severity, const char *fmt, ...) {
    if(!g_capturing) {
        if(severity < 0) g_after_fix_debug++;
        return;
    }
    if(severity == 0) g_fix_warnings++;
    if(severity > 0) g_fix_fatals++;
    if(severity >= 0) return;
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if(n < 0) {
        va_end(ap2);
        return;
    }
    char *m = malloc((size_t)n + 1);
    vsnprintf(m, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    parse_event(m);
    free(m);
}

/* Assignment lookups over the walk. */
static const char *type_file(int id) {
    const asn1p_expr_t *e = asn1viz_schema_expr(id);
    return e && e->module ? base_name(e->module->source_file_name) : "";
}

static int type_by_name(const char *file, const char *name) {
    for(int i = 0; i < asn1viz_schema_types(); i++) {
        const asn1p_expr_t *e = asn1viz_schema_expr(i);
        if(e->Identifier && !strcmp(e->Identifier, name) && (!file || !strcmp(type_file(i), file))) return i;
    }
    return -1;
}

static int type_by_line(const char *file, int line) {
    int best = -1, best_line = -1;
    for(int i = 0; i < asn1viz_schema_types(); i++) {
        const asn1p_expr_t *e = asn1viz_schema_expr(i);
        if(strcmp(type_file(i), file)) continue;
        if(e->_lineno <= line && e->_lineno > best_line) best = i, best_line = e->_lineno;
    }
    return best;
}

/* Each event's assignment, by the first rule that applies (recorded as
 * `how`): "processing", the "=== Now processing" marker's name; "line", the
 * assignment whose source lines hold the line the message names; "name",
 * the assignment the message names in parentheses; "current", the last
 * assignment attributed in the same schema file. Events in asn1c's
 * standard modules (not in the schema) stay unattributed ("none"). */
static void attribute_events(int *how_counts) {
    /* The last assignment seen per schema file. */
    const char **files = calloc((size_t)nEV + 1, sizeof *files);
    int *last = calloc((size_t)nEV + 1, sizeof *last);
    int nfiles = 0;
    for(int i = 0; i < nEV; i++) {
        event_t *e = &EV[i];
        const char *file = e->file ? e->file : "";
        int f = 0;
        while(f < nfiles && strcmp(files[f], file)) f++;
        if(f == nfiles) {
            files[nfiles] = file;
            last[nfiles++] = -1;
        }
        if(e->now) {
            e->type = e->expr ? type_by_name(file, e->expr) : -1;
            e->how = 'p';
        } else if(e->line >= 0 && (e->type = type_by_line(file, e->line)) >= 0) {
            e->how = 'l';
        } else if(e->expr && (e->type = type_by_name(file, e->expr)) >= 0) {
            e->how = 'n';
        } else if(last[f] >= 0) {
            e->type = last[f];
            e->how = 'c';
        }
        if(e->type >= 0 || e->now) last[f] = e->type;
        switch(e->how) {
        case 'p': how_counts[0]++; break;
        case 'l': how_counts[1]++; break;
        case 'n': how_counts[2]++; break;
        case 'c': how_counts[3]++; break;
        default: how_counts[4]++; break;
        }
    }
    free(files);
    free(last);
}

/* ---------------------------------------------------------------- standard modules */

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int copy_file(const char *src, const char *dst);

/* asn1c's importStandardModules (asn1c/asn1c.c), in name order. */
static int import_standard_modules(asn1p_t *asn, const char *skeletons) {
    char dir[4096];
    snprintf(dir, sizeof dir, "%s/standard-modules", skeletons);
    DIR *d = opendir(dir);
    if(!d) return -1;
    char **names = NULL;
    int n = 0;
    struct dirent *de;
    while((de = readdir(d))) {
        size_t l = strlen(de->d_name);
        if(l <= 5 || strcmp(de->d_name + l - 5, ".asn1")) continue;
        names = realloc(names, (size_t)(n + 1) * sizeof *names);
        names[n++] = xstrdup(de->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof *names, cmp_str);
    /* Parsed from copies in the work directory, by a relative path: asn1c
     * writes the module's path into the code it generates for it. */
    if(mkdir("standard-modules", 0700)) return -1;
    int ret = 0;
    for(int i = 0; i < n; i++) {
        char src[8192], path[8192];
        snprintf(src, sizeof src, "%s/%s", dir, names[i]);
        snprintf(path, sizeof path, "standard-modules/%s", names[i]);
        if(copy_file(src, path)) {
            ret = -1;
            continue;
        }
        asn1p_t *s = asn1p_parse_file(path, A1P_NOFLAGS);
        if(!s) {
            ret = -1;
            continue;
        }
        asn1p_module_t *mod;
        while((mod = TQ_REMOVE(&s->modules, mod_next))) {
            mod->_tags |= MT_STANDARD_MODULE;
            TQ_ADD(&asn->modules, mod, mod_next);
        }
    }
    return ret;
}

/* ---------------------------------------------------------------- the generated files */

typedef struct {
    char *name;
    long count;
} hist_t;

typedef struct {
    hist_t *v;
    int n;
} histo_t;

static void histo_add(histo_t *h, const char *name, long by) {
    for(int i = 0; i < h->n; i++)
        if(!strcmp(h->v[i].name, name)) {
            h->v[i].count += by;
            return;
        }
    h->v = realloc(h->v, (size_t)(h->n + 1) * sizeof *h->v);
    h->v[h->n].name = xstrdup(name);
    h->v[h->n].count = by;
    h->n++;
}

static int cmp_hist(const void *a, const void *b) {
    const hist_t *x = a, *y = b;
    if(x->count != y->count) return x->count > y->count ? -1 : 1;
    return strcmp(x->name, y->name);
}

static void histo_sort(histo_t *h) {
    if(h->n) qsort(h->v, (size_t)h->n, sizeof *h->v, cmp_hist);
}

typedef struct {
    char *path;
    long bytes;
    int lines;
    int unannotated;
    histo_t by_fn, by_src;
    int type;
    char group; /* 'c'ompiled, 's'keleton, 'g'enerated */
} gfile_t;

/* "\t// asn1c_C.c:4166 emit_member_table()" at the end of a line: its
 * length, or 0. */
static size_t origin_of(const char *line, char *src, size_t srcsz, char *fn, size_t fnsz) {
    const char *o = NULL;
    for(const char *q = strstr(line, "\t// "); q; q = strstr(q + 1, "\t// ")) o = q;
    if(!o) return 0;
    const size_t comment = strlen(o);
    o += 4;
    const char *colon = strchr(o, ':');
    const char *sp = colon ? strchr(colon, ' ') : NULL;
    if(!colon || !sp) return 0;
    for(const char *q = o; q < colon; q++)
        if(*q == ' ') return 0;
    for(const char *q = colon + 1; q < sp; q++)
        if(*q < '0' || *q > '9') return 0;
    const char *paren = strstr(sp, "()");
    if(!paren || paren[2] != 0 || paren == sp + 1) return 0;
    for(const char *q = sp + 1; q < paren; q++)
        if(!is_ident_char(*q) || *q == '-') return 0;
    const char *file = o;
    for(const char *q = o; q < colon; q++)
        if(*q == '/') file = q + 1; /* __FILE__ is a build path */
    snprintf(src, srcsz, "%.*s:%d", (int)(colon - file), file, atoi(colon + 1));
    snprintf(fn, fnsz, "%.*s", (int)(paren - sp - 1), sp + 1);
    return comment;
}

static void scan_file(gfile_t *f, const char *text, size_t len) {
    f->bytes = (long)len;
    const char *s = text, *end = text + len;
    char line[16384], src[512], fn[256];
    while(s < end) {
        const char *nl = memchr(s, '\n', (size_t)(end - s));
        size_t l = nl ? (size_t)(nl - s) : (size_t)(end - s);
        snprintf(line, sizeof line, "%.*s", (int)(l < sizeof line ? l : sizeof line - 1), s);
        f->lines++;
        size_t comment = origin_of(line, src, sizeof src, fn, sizeof fn);
        if(comment) {
            /* The comment holds asn1c's __FILE__, a build path: the size
             * kept is the code's, as asn1c writes it without the comments. */
            f->bytes -= (long)comment;
            histo_add(&f->by_fn, fn, 1);
            histo_add(&f->by_src, src, 1);
        } else {
            f->unannotated++;
        }
        s = nl ? nl + 1 : end;
    }
    histo_sort(&f->by_fn);
    histo_sort(&f->by_src);
}

static int cmp_gfile(const void *a, const void *b) { return strcmp(((const gfile_t *)a)->path, ((const gfile_t *)b)->path); }

/* The names the compile messages give: "Compiled X.c[ (contents
 * unchanged)]" and "Copied PATH\t-> X[ note]" (or Symlinked). */
static int in_list(char **list, int n, const char *name) {
    for(int i = 0; i < n; i++)
        if(!strcmp(list[i], name)) return 1;
    return 0;
}

typedef struct {
    asn1p_t *asn;
    const char *skeletons;
    uint64_t flags;
    int argc, optc;
    char **argv;
} compile_ctx_t;

static int run_compile(void *p) {
    compile_ctx_t *c = p;
    return asn1_compile(c->asn, c->skeletons, "", (enum asn1c_flags)c->flags, c->argc, c->optc, c->argv, 4);
}

static void remove_tree(const char *dir) {
    DIR *d = opendir(dir);
    if(d) {
        struct dirent *de;
        while((de = readdir(d))) {
            if(!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char path[8192];
            snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
            struct stat st;
            if(lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(path);
            else unlink(path);
        }
        closedir(d);
    }
    rmdir(dir);
}

static const char *absolute(const char *cwd, const char *p) {
    if(p[0] == '/') return p;
    size_t n = strlen(cwd) + strlen(p) + 2;
    char *a = malloc(n);
    snprintf(a, n, "%s/%s", cwd, p);
    return a;
}

/* Copies a file; fails if dst exists. */
static int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if(!in) return -1;
    FILE *out = fopen(dst, "wbx");
    if(!out) {
        fclose(in);
        return -1;
    }
    size_t len;
    char *text = read_stream(in, &len);
    int rc = fwrite(text, 1, len, out) == len ? 0 : -1;
    free(text);
    fclose(in);
    if(fclose(out)) rc = -1;
    return rc;
}

/* ---------------------------------------------------------------- main */

static void usage(void) {
    fprintf(stderr, "usage: " PROG " -S SKELETONS --out TRACE.json [--schema-out SCHEMA.json] [--schema-name NAME] "
                    "SCHEMA.asn1...\n");
}

static void write_hist(FILE *out, const histo_t *h) {
    fputc('{', out);
    for(int i = 0; i < h->n; i++) {
        fputs(i ? ", " : " ", out);
        asn1viz_json_string(out, h->v[i].name);
        fprintf(out, ": %ld", h->v[i].count);
    }
    fputs(h->n ? " }" : "}", out);
}

int main(int argc, char **argv) {
    const char *out_path = NULL, *schema_out = NULL, *schema_name = NULL, *skeletons = NULL;
    char **inputs = NULL;
    int ninputs = 0;
    for(int i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else if(!strcmp(argv[i], "--schema-out") && i + 1 < argc) schema_out = argv[++i];
        else if(!strcmp(argv[i], "--schema-name") && i + 1 < argc) schema_name = argv[++i];
        else if(!strcmp(argv[i], "-S") && i + 1 < argc) skeletons = argv[++i];
        else if(argv[i][0] == '-') {
            usage();
            return 2;
        } else {
            inputs = realloc(inputs, (size_t)(ninputs + 1) * sizeof *inputs);
            inputs[ninputs++] = argv[i];
        }
    }
    if(!out_path || !skeletons || !ninputs) {
        usage();
        return 2;
    }
    char *skel_abs = realpath(skeletons, NULL);
    if(!skel_abs) {
        fprintf(stderr, PROG ": no skeletons at %s\n", skeletons);
        return 1;
    }
    if(!schema_name) schema_name = schema_out ? base_name(schema_out) : NULL;

    /* A work directory: the inputs are copied in and parsed by their
     * basenames, because asn1c writes the schema path into the generated
     * code ("found in ..."), and the trace must not depend on where the
     * build tree is. The compiler writes into out/. */
    char cwd[8192], dir[4096];
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/asn1-viz-compile-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if(!getcwd(cwd, sizeof cwd) || !mkdtemp(dir)) {
        fprintf(stderr, PROG ": no temporary directory: %s\n", strerror(errno));
        return 1;
    }
    out_path = absolute(cwd, out_path);
    if(schema_out) schema_out = absolute(cwd, schema_out);
    for(int i = 0; i < ninputs; i++) {
        char dst[8192];
        snprintf(dst, sizeof dst, "%s/%s", dir, base_name(inputs[i]));
        if(copy_file(inputs[i], dst)) {
            fprintf(stderr, PROG ": cannot read %s (or two inputs share a name)\n", inputs[i]);
            remove_tree(dir);
            return 1;
        }
        inputs[i] = (char *)base_name(inputs[i]);
    }
    char outdir[8192];
    snprintf(outdir, sizeof outdir, "%s/out", dir);
    if(mkdir(outdir, 0700) || chdir(dir)) {
        fprintf(stderr, PROG ": cannot use %s\n", dir);
        remove_tree(dir);
        return 1;
    }

    const char *parse_status = "ok", *fix_status = "skipped", *compile_status = "skipped", *save_status = "skipped";

    /* Parse. */
    asn1p_t *asn = NULL;
    for(int i = 0; i < ninputs; i++) {
        asn1p_t *a = asn1p_parse_file(inputs[i], A1P_NOFLAGS);
        if(!a) {
            fprintf(stderr, PROG ": cannot parse %s\n", inputs[i]);
            parse_status = "failed";
            break;
        }
        if(!asn) {
            asn = a;
        } else {
            asn1p_module_t *mod;
            while((mod = TQ_REMOVE(&a->modules, mod_next))) TQ_ADD(&asn->modules, mod, mod_next);
        }
    }
    int ok = !strcmp(parse_status, "ok");

    /* The parse order, and the tree printed before fixing. */
    asn1p_expr_t **parsed = NULL;
    int nparsed = 0;
    printed_t before = {0}, after = {0};
    int fx = 0, nassign = 0, how[5] = {0};
    if(ok) {
        asn1p_module_t *mod;
        TQ_FOR(mod, &asn->modules, mod_next) {
            asn1p_expr_t *e;
            TQ_FOR(e, &mod->members, next) {
                parsed = realloc(parsed, (size_t)(nparsed + 1) * sizeof *parsed);
                parsed[nparsed++] = e;
            }
        }
        before = print_tree(asn, parsed, nparsed);
    }

    /* Fix, as the driver does: the standard modules imported first. */
    if(ok) {
        if(import_standard_modules(asn, skel_abs) == 0) asn1f_use_standard_namespaces(asn);
        g_capturing = 1;
        fx = asn1f_process(asn, A1F_DEBUG, fixer_logger);
        g_capturing = 0;
        fix_status = fx < 0 ? "failed" : "ok";
        ok = fx >= 0;
        if(!ok) how[4] = nEV; /* no walk of a broken tree, so no attribution */
    }

    /* The walk (the ids asn1-schema/1 uses), the events attributed, and the
     * tree printed again. */
    char **diffs = NULL;
    int removed = 0, added = 0, diff_types = 0;
    if(ok) {
        nassign = asn1viz_schema_walk(asn);
        attribute_events(how);
        asn1p_expr_t **walked = malloc(((size_t)nassign + 1) * sizeof *walked);
        for(int i = 0; i < nassign; i++) walked[i] = asn1viz_schema_expr(i);
        after = print_tree(asn, walked, nassign);
        diffs = calloc((size_t)nassign + 1, sizeof *diffs);
        if(before.ok && after.ok) {
            for(int i = 0; i < nassign; i++) {
                /* The parsed assignment for this id: the same node (the
                 * fixer rewrites nodes in place), else the same name and
                 * module. */
                int k = -1;
                for(int j = 0; j < nparsed && k < 0; j++)
                    if(parsed[j] == walked[i]) k = j;
                for(int j = 0; j < nparsed && k < 0; j++)
                    if(parsed[j]->Identifier && walked[i]->Identifier
                       && !strcmp(parsed[j]->Identifier, walked[i]->Identifier) && parsed[j]->module == walked[i]->module)
                        k = j;
                if(k < 0 || (!before.text[k] && !after.text[i])) continue;
                diffs[i] = unified_diff(before.text[k], after.text[i], &removed, &added);
                if(*diffs[i]) diff_types++;
            }
        }
    }

    /* Compile, into a temporary directory. */
    gfile_t *G = NULL;
    int nG = 0, compile_rc = 0;
    const uint64_t cflags = (uint64_t)A1C_NO_C99 | A1C_GEN_BER | A1C_GEN_XER | A1C_GEN_OER | A1C_GEN_UPER | A1C_GEN_APER
                       | A1C_GEN_PRINT | A1C_GEN_RFILL | A1C_GEN_EXAMPLE | A1C_GEN_JER | A1C_GEN_CBOR | A1C_GEN_CER
                       | A1C_DEBUG_OUTPUT_ORIGIN_LINES;
    if(ok) {
        if(chdir("out")) {
            fprintf(stderr, PROG ": cannot use %s/out\n", dir);
            compile_status = "failed";
        } else {
            /* The driver's argv: the inputs after the options. */
            char *cargv[256];
            int cargc = 0;
            cargv[cargc++] = (char *)"asn1c";
            cargv[cargc++] = (char *)"-debug-output-origin-lines";
            int optc = cargc;
            for(int i = 0; i < ninputs && cargc < 255; i++) cargv[cargc++] = inputs[i];
            cargv[cargc] = NULL;
            compile_ctx_t c = {asn, skel_abs, cflags, cargc, optc, cargv};
            char *log = capture_fd(2, run_compile, &c, &compile_rc);
            compile_status = compile_rc ? "failed" : "ok";
            save_status = compile_rc ? "failed" : "ok";
            /* What the messages say each file is. */
            char **compiled = NULL, **copied = NULL;
            int ncompiled = 0, ncopied = 0;
            lines_t L = split_lines(log);
            for(int i = 0; i < L.n; i++) {
                char *l = L.v[i];
                if(!strncmp(l, "Compiled ", 9)) {
                    char *name = l + 9, *sp = strchr(name, ' ');
                    if(sp) *sp = 0;
                    compiled = realloc(compiled, (size_t)(ncompiled + 1) * sizeof *compiled);
                    compiled[ncompiled++] = xstrdup(base_name(name));
                } else if(!strncmp(l, "Copied ", 7) || !strncmp(l, "Symlinked ", 10)) {
                    char *arrow = strstr(l, "\t-> ");
                    if(!arrow) continue;
                    char *name = arrow + 4, *sp = strchr(name, ' ');
                    if(sp) *sp = 0;
                    copied = realloc(copied, (size_t)(ncopied + 1) * sizeof *copied);
                    copied[ncopied++] = xstrdup(base_name(name));
                }
            }
            free(log);
            /* Every file in the directory. */
            DIR *d = opendir(".");
            struct dirent *de;
            while(d && (de = readdir(d))) {
                struct stat st;
                if(stat(de->d_name, &st) || !S_ISREG(st.st_mode)) continue;
                FILE *f = fopen(de->d_name, "rb");
                if(!f) continue;
                size_t len;
                char *text = read_stream(f, &len);
                fclose(f);
                G = realloc(G, (size_t)(nG + 1) * sizeof *G);
                gfile_t *g = &G[nG++];
                memset(g, 0, sizeof *g);
                g->path = xstrdup(de->d_name);
                g->type = -1;
                g->group = in_list(compiled, ncompiled, g->path) ? 'c' : in_list(copied, ncopied, g->path) ? 's' : 'g';
                if(g->group == 's') g->bytes = (long)len;
                else scan_file(g, text, len);
                free(text);
            }
            if(d) closedir(d);
            if(nG) qsort(G, (size_t)nG, sizeof *G, cmp_gfile);
            if(chdir("..")) fprintf(stderr, PROG ": cannot return to %s\n", dir);
        }
    }
    if(chdir(cwd)) fprintf(stderr, PROG ": cannot return to %s\n", cwd);
    remove_tree(dir);

    /* Files to assignments, by asn1c's own file naming (asn1c_save.c). */
    long *code_bytes = NULL;
    if(ok && !strcmp(compile_status, "ok")) {
        code_bytes = calloc((size_t)nassign + 1, sizeof *code_bytes);
        for(int i = 0; i < nassign; i++) {
            asn1p_expr_t *e = asn1viz_schema_expr(i);
            if(!e->Identifier) continue;
            char *id = xstrdup(asn1c_make_identifier(AMI_MASK_ONLY_SPACES | AMI_USE_PREFIX, e, (char *)0));
            const char *stem = asn1c_disambiguate_generated_filename(id);
            for(int k = 0; k < nG; k++) {
                if(G[k].group != 'c' || G[k].type >= 0) continue;
                size_t sl = strlen(stem);
                if(!strncmp(G[k].path, stem, sl) && (!strcmp(G[k].path + sl, ".c") || !strcmp(G[k].path + sl, ".h"))) {
                    G[k].type = i;
                    code_bytes[i] += G[k].bytes;
                }
            }
            free(id);
        }
    }

    /* asn1-schema/1, with codeBytes. */
    if(ok && schema_out) {
        FILE *s = fopen(schema_out, "wb");
        if(!s) {
            fprintf(stderr, PROG ": cannot write %s\n", schema_out);
            return 1;
        }
        asn1viz_schema_write(s, ASN1VIZ_ASN1C_COMMIT, fx, g_fix_warnings, code_bytes);
        fclose(s);
    }

    /* asn1-compile/1. */
    FILE *out = fopen(out_path, "wb");
    if(!out) {
        fprintf(stderr, PROG ": cannot write %s\n", out_path);
        return 1;
    }
    fprintf(out, "{\n  \"format\": \"asn1-compile/1\",\n  \"asn1c\": \"%s\",\n", ASN1VIZ_ASN1C_COMMIT);
    fprintf(out, "  \"flags\": [\"-debug-output-origin-lines\"],\n  \"schema\": ");
    if(ok && schema_name) asn1viz_json_string(out, schema_name);
    else fputs("null", out);
    fprintf(out, ",\n  \"inputs\": [");
    for(int i = 0; i < ninputs; i++) {
        fputs(i ? ", " : "", out);
        asn1viz_json_string(out, base_name(inputs[i]));
    }
    fprintf(out, "],\n  \"parseStatus\": \"%s\",\n  \"parse\": [", parse_status);
    /* The parse order in walk ids (the walk and the parse visit the same
     * nodes in the same order). */
    for(int j = 0, w = 0; ok && j < nparsed; j++) {
        int id = -1;
        for(int i = 0; i < nassign && id < 0; i++)
            if(asn1viz_schema_expr(i) == parsed[j]) id = i;
        fprintf(out, "%s\n    { \"type\": ", w++ ? "," : "");
        if(id >= 0) fprintf(out, "%d", id);
        else fputs("null", out);
        fprintf(out, ", \"line\": %d }", parsed[j]->_lineno);
    }
    fprintf(out, "\n  ],\n");

    /* fix */
    fprintf(out, "  \"fix\": {\n    \"status\": \"%s\", \"result\": %d, \"warnings\": %d, \"fatals\": %d,\n", fix_status, fx,
            g_fix_warnings, g_fix_fatals);
    fprintf(out, "    \"attribution\": { \"processing\": %d, \"line\": %d, \"name\": %d, \"current\": %d, \"none\": %d },\n",
            how[0], how[1], how[2], how[3], how[4]);
    fprintf(out, "    \"printed\": { \"before\": %d, \"after\": %d, \"removed\": %d, \"added\": %d, \"assignments\": %d },\n",
            before.lines, after.lines, removed, added, diff_types);
    fprintf(out, "    \"events\": [");
    for(int i = 0; i < nEV; i++) {
        const event_t *e = &EV[i];
        fprintf(out, "%s\n      {\"seq\":%d,\"fn\":", i ? "," : "", i);
        asn1viz_json_string(out, e->fn);
        fputs(",\"type\":", out);
        if(e->type >= 0) fprintf(out, "%d", e->type);
        else fputs("null", out);
        fputs(",\"line\":", out);
        if(e->line >= 0) fprintf(out, "%d", e->line);
        else fputs("null", out);
        fputs(",\"src\":", out);
        if(e->src) asn1viz_json_string(out, e->src);
        else fputs("null", out);
        if(e->expr) {
            fputs(",\"expr\":", out);
            asn1viz_json_string(out, e->expr);
        }
        fprintf(out, ",\"how\":\"%s\"}",
                e->how == 'p' ? "processing" : e->how == 'l' ? "line" : e->how == 'n' ? "name" : e->how == 'c' ? "current" : "none");
    }
    fprintf(out, "\n    ],\n    \"diffs\": {");
    for(int i = 0, w = 0; diffs && i < nassign; i++) {
        if(!diffs[i] || !*diffs[i]) continue;
        fprintf(out, "%s\n      \"%d\": ", w++ ? "," : "", i);
        asn1viz_json_string(out, diffs[i]);
    }
    fprintf(out, "\n    }\n  },\n");

    /* compile */
    histo_t fns = {0};
    long total_lines = 0, annotated = 0, gen_files = 0, attributed = 0;
    for(int k = 0; k < nG; k++) {
        if(G[k].group != 'c') continue;
        gen_files++;
        total_lines += G[k].lines;
        annotated += G[k].lines - G[k].unannotated;
        if(G[k].type >= 0) attributed++;
        for(int h = 0; h < G[k].by_fn.n; h++) histo_add(&fns, G[k].by_fn.v[h].name, G[k].by_fn.v[h].count);
    }
    histo_sort(&fns);
    histo_t srcs = {0};
    for(int k = 0; k < nG; k++)
        if(G[k].group == 'c')
            for(int h = 0; h < G[k].by_src.n; h++) histo_add(&srcs, G[k].by_src.v[h].name, G[k].by_src.v[h].count);
    fprintf(out, "  \"compile\": {\n    \"status\": \"%s\", \"result\": %d,\n", compile_status, compile_rc);
    fprintf(out, "    \"totals\": { \"files\": %ld, \"lines\": %ld, \"annotated\": %ld, \"attributed\": %ld, \"sources\": %d },\n",
            gen_files, total_lines, annotated, attributed, srcs.n);
    fprintf(out, "    \"functions\": [");
    for(int h = 0; h < fns.n; h++) {
        fputs(h ? ", " : "", out);
        asn1viz_json_string(out, fns.v[h].name);
    }
    fprintf(out, "],\n    \"byFn\": ");
    write_hist(out, &fns);
    fprintf(out, ",\n    \"files\": [");
    for(int k = 0, w = 0; k < nG; k++) {
        const gfile_t *g = &G[k];
        if(g->group != 'c') continue;
        fprintf(out, "%s\n      { \"path\": ", w++ ? "," : "");
        asn1viz_json_string(out, g->path);
        fputs(", \"type\": ", out);
        if(g->type >= 0) fprintf(out, "%d", g->type);
        else fputs("null", out);
        fprintf(out, ", \"lines\": %d, \"bytes\": %ld, \"unannotated\": %d,\n        \"byFn\": ", g->lines, g->bytes,
                g->unannotated);
        write_hist(out, &g->by_fn);
        fputs(",\n        \"bySrc\": ", out);
        write_hist(out, &g->by_src);
        fputs(" }", out);
    }
    fprintf(out, "\n    ]\n  },\n");

    /* save */
    fprintf(out, "  \"save\": {\n    \"status\": \"%s\",\n    \"skeletons\": [", save_status);
    for(int k = 0, w = 0; k < nG; k++) {
        if(G[k].group != 's') continue;
        fprintf(out, "%s\n      { \"path\": ", w++ ? "," : "");
        asn1viz_json_string(out, G[k].path);
        fprintf(out, ", \"bytes\": %ld }", G[k].bytes);
    }
    fprintf(out, "\n    ],\n    \"generated\": [");
    for(int k = 0, w = 0; k < nG; k++) {
        if(G[k].group != 'g') continue;
        fprintf(out, "%s\n      { \"path\": ", w++ ? "," : "");
        asn1viz_json_string(out, G[k].path);
        fprintf(out, ", \"bytes\": %ld, \"lines\": %d }", G[k].bytes, G[k].lines);
    }
    fprintf(out, "\n    ]\n  }\n}\n");
    fclose(out);

    int nskel = 0;
    for(int k = 0; k < nG; k++) nskel += G[k].group == 's';
    fprintf(stderr,
            PROG ": parse %s (%d assignments); fix %s (%d events, %d unattributed; %d of %d printed lines changed in %d "
                 "assignments); compile %s (%ld files, %ld lines, %ld annotated, %d functions); save %s (%d skeletons)\n",
            parse_status, nparsed, fix_status, nEV, how[4], removed, before.lines, diff_types, compile_status, gen_files,
            total_lines, annotated, fns.n, save_status, nskel);
    return 0;
}
