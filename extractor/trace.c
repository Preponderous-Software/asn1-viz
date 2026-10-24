/* The asn1-viz tracer. See trace.h and RFC 0023 section 2. */
#include "trace.h"

#include <stdlib.h>
#include <string.h>

#include "aper_encoder.h"
#include "aper_support.h"
#include "asn_bit_data.h"
#include "constr_CHOICE.h"
#include "constr_SEQUENCE.h"
#include "constr_SET_OF.h"
#include "asn_SET_OF.h"
#include "per_support.h"
#include "uper_encoder.h"
#include "uper_support.h"

/* The constructed types' op tables, compared by address to classify a frame.
 * Weak, because asn1c only copies the skeletons a schema needs, and a table
 * that is not linked in cannot be any frame's table. */
extern asn_TYPE_operation_t asn_OP_SEQUENCE __attribute__((weak));
extern asn_TYPE_operation_t asn_OP_SET __attribute__((weak));
extern asn_TYPE_operation_t asn_OP_CHOICE __attribute__((weak));
extern asn_TYPE_operation_t asn_OP_SEQUENCE_OF __attribute__((weak));
extern asn_TYPE_operation_t asn_OP_SET_OF __attribute__((weak));

enum { K_LEAF, K_SEQUENCE, K_CHOICE, K_LIST };

static int kind_of(const asn_TYPE_descriptor_t *td) {
    const asn_TYPE_operation_t *op = td->op;
    if(!op) return K_LEAF;
    if((&asn_OP_SEQUENCE && op == &asn_OP_SEQUENCE) || (&asn_OP_SET && op == &asn_OP_SET)) return K_SEQUENCE;
    if(&asn_OP_CHOICE && op == &asn_OP_CHOICE) return K_CHOICE;
    if((&asn_OP_SEQUENCE_OF && op == &asn_OP_SEQUENCE_OF) || (&asn_OP_SET_OF && op == &asn_OP_SET_OF)) return K_LIST;
    return K_LEAF;
}

static size_t pos_of(const asn_per_outp_t *po) {
    return po->flushed_bytes * 8 + (size_t)(po->buffer - po->tmpspace) * 8 + po->nboff;
}

/* ------------------------------------------------------------------ op tables */

typedef struct {
    asn_TYPE_operation_t *op;
    per_type_encoder_f *uper, *aper;
} orig_t;

static orig_t *g_origs;
static size_t g_norigs, g_corigs;
static int g_installed;

static orig_t *find_orig(const asn_TYPE_operation_t *op) {
    for(size_t i = 0; i < g_norigs; i++)
        if(g_origs[i].op == op) return &g_origs[i];
    return NULL;
}

static asn_enc_rval_t shim_uper(const asn_TYPE_descriptor_t *, const asn_per_constraints_t *, const void *,
                                asn_per_outp_t *);
static asn_enc_rval_t shim_aper(const asn_TYPE_descriptor_t *, const asn_per_constraints_t *, const void *,
                                asn_per_outp_t *);

static const asn_TYPE_descriptor_t **g_seen;
static size_t g_nseen, g_cseen;

static void patch(asn_TYPE_descriptor_t *td) {
    if(!td || !td->op) return;
    for(size_t k = 0; k < g_nseen; k++)
        if(g_seen[k] == td) return; /* visited: recursive types end here */
    if(g_nseen == g_cseen) {
        g_cseen = g_cseen ? g_cseen * 2 : 64;
        g_seen = realloc(g_seen, g_cseen * sizeof *g_seen);
    }
    g_seen[g_nseen++] = td;
    if(!find_orig(td->op)) {
        if(g_norigs == g_corigs) {
            g_corigs = g_corigs ? g_corigs * 2 : 32;
            g_origs = realloc(g_origs, g_corigs * sizeof *g_origs);
        }
        orig_t *o = &g_origs[g_norigs++];
        o->op = td->op;
        o->uper = td->op->uper_encoder;
        o->aper = td->op->aper_encoder;
        if(o->uper) td->op->uper_encoder = shim_uper;
        if(o->aper) td->op->aper_encoder = shim_aper;
    }
    for(unsigned i = 0; i < td->elements_count; i++) patch(td->elements[i].type);
}

static asn_TYPE_descriptor_t *g_pdu;

int asn1viz_trace_install(asn_TYPE_descriptor_t *pdu) {
    if(g_installed) asn1viz_trace_restore();
    g_pdu = pdu;
    g_nseen = 0;
    patch(pdu);
    g_installed = 1;
    return (int)g_norigs;
}

void asn1viz_trace_restore(void) {
    for(size_t i = 0; i < g_norigs; i++) {
        g_origs[i].op->uper_encoder = g_origs[i].uper;
        g_origs[i].op->aper_encoder = g_origs[i].aper;
    }
    g_norigs = 0;
    g_installed = 0;
}

/* ------------------------------------------------------------------ recording */

typedef struct {
    const asn_TYPE_descriptor_t *td;
    const void *sptr;
    const asn_per_constraints_t *ct; /* effective PER constraints */
    asn_per_outp_t *po;
    int buf;
    int parent;
    size_t start, end;
    char *path;
    int depth;
    int nchildren;
    int pre_writes;   /* unlabelled writes before the first member (SEQUENCE/CHOICE preamble) */
    int saw_nslength; /* SEQUENCE: the extension bitmap's length was written */
    int ext_bits;     /* SEQUENCE: extension bitmap bits written so far */
    int leaf_writes;  /* leaf: unlabelled writes so far */
    int leaf_ext;     /* leaf: its first write was an extension bit */
} fr_t;

typedef struct {
    int buf;
    size_t pos;
    size_t n;
    int frame;
    const char *kind;
    int source;
    const char *member;
    int drop;      /* an open type's octets, replaced by the open type's own segments */
    uint32_t val;  /* few-bit writes: the bits written */
    int few;
} wr_t;

typedef struct {
    int parent_buf;
    int sorted;          /* a SET OF element encoded for sorting: used only if its octets are relayed */
    const void *root_sptr;
    int has_reloc;
    size_t reloc;        /* where bit 0 of this buffer lands in the parent buffer */
    size_t content_bits; /* octets * 8 carried in the parent */
    int content_frame;   /* the parent frame that carried them */
} buf_t;

/* A separate-buffer context: an open type (one inner encode) or a SET OF
 * sorting pass (one inner encode per element). */
typedef struct {
    int sorting;
    asn_per_outp_t *outer_po;
    int outer_buf;
    int inner_buf; /* open type: -1 until the inner encode starts */
    size_t map_mark;
    int content_writes;
} ot_t;

/* SET OF elements awaiting their octets: the element buffer asn1c will
 * relay, and the tracer buffer that recorded its encoding. */
typedef struct {
    const uint8_t *src;
    int buf;
} pending_t;

typedef struct {
    asn_per_outp_t *po;
    int buf;
} map_t;

static struct {
    int recording;
    int codec;
    fr_t *frames;
    size_t nframes, cframes;
    int stack[512];
    int depth;
    wr_t *writes;
    size_t nwrites, cwrites;
    buf_t bufs[4096];
    int nbufs;
    ot_t ots[64];
    int nots;
    map_t map[256];
    size_t nmap;
    pending_t pending[1024];
    size_t npending;
    /* The outermost labelled helper in progress. */
    const char *helper;
    int helper_depth;
    asn_per_outp_t *helper_po;
    size_t helper_first; /* index of its first write */
    int write_depth;
    char error[256];
} R;

static void fail(const char *msg) {
    if(!R.error[0]) snprintf(R.error, sizeof R.error, "%s", msg);
}

static void map_add(asn_per_outp_t *po, int b) {
    if(R.nmap < 256) {
        R.map[R.nmap].po = po;
        R.map[R.nmap].buf = b;
        R.nmap++;
    } else {
        fail("too many output streams at once");
    }
}

/* The buffer a stream writes into: the latest one opened for it. */
static int buf_for(asn_per_outp_t *po) {
    for(size_t i = R.nmap; i-- > 0;)
        if(R.map[i].po == po) return R.map[i].buf;
    if(R.nbufs == 0) { /* the root stream */
        R.bufs[0].parent_buf = -1;
        R.nbufs = 1;
        map_add(po, 0);
        return 0;
    }
    char msg[256];
    snprintf(msg, sizeof msg,
             "a write on an output stream the tracer cannot place (not the root and not an open type), inside %s",
             R.depth ? R.frames[R.stack[R.depth - 1]].path : "(no frame)");
    fail(msg);
    return 0;
}

/* A new inner encode on its own stream, inside an open type or a SET OF
 * sorting pass (a stream address can be reused by the next encode, so every
 * inner encode gets its own buffer). */
static int new_buffer(asn_per_outp_t *po, const void *root_sptr) {
    if(R.nots == 0 || R.nbufs >= 4096) {
        fail("an encode on a separate stream outside any open type or SET OF");
        return 0;
    }
    ot_t *ot = &R.ots[R.nots - 1];
    int b = R.nbufs++;
    memset(&R.bufs[b], 0, sizeof R.bufs[b]);
    R.bufs[b].parent_buf = ot->outer_buf;
    R.bufs[b].sorted = ot->sorting;
    R.bufs[b].root_sptr = root_sptr;
    if(!ot->sorting) {
        if(ot->inner_buf >= 0) fail("an open type encoded twice");
        ot->inner_buf = b;
    }
    map_add(po, b);
    return b;
}

static char *child_path(const fr_t *p, const asn_TYPE_descriptor_t *td, const void *sptr) {
    char tmp[1024];
    if(!p) {
        snprintf(tmp, sizeof tmp, "%s", td->name);
        return strdup(tmp);
    }
    const asn_TYPE_descriptor_t *pt = p->td;
    switch(kind_of(pt)) {
    case K_CHOICE: {
        const asn_CHOICE_specifics_t *cs = pt->specifics;
        const char *base = (const char *)p->sptr + cs->pres_offset;
        unsigned pres = 0;
        switch(cs->pres_size) {
        case 1: pres = *(const uint8_t *)base; break;
        case 2: pres = *(const uint16_t *)base; break;
        case 4: pres = *(const uint32_t *)base; break;
        default: pres = (unsigned)*(const uint64_t *)base; break;
        }
        if(pres >= 1 && pres <= pt->elements_count) {
            snprintf(tmp, sizeof tmp, "%s.%s", p->path, pt->elements[pres - 1].name);
            return strdup(tmp);
        }
        break;
    }
    case K_LIST: {
        /* The element's index in the value (SET OF elements are written in sorted order). */
        const asn_anonymous_set_ *list = _A_CSET_FROM_VOID(p->sptr);
        int idx = p->nchildren;
        for(int i = 0; list && i < list->count; i++)
            if(list->array[i] == sptr) {
                idx = i;
                break;
            }
        snprintf(tmp, sizeof tmp, "%s[%d]", p->path, idx);
        return strdup(tmp);
    }
    case K_SEQUENCE: {
        const char *base = p->sptr;
        const asn_TYPE_member_t *best = NULL;
        for(unsigned i = 0; i < pt->elements_count; i++) {
            const asn_TYPE_member_t *e = &pt->elements[i];
            const char *m = base + e->memb_offset;
            if((e->flags & ATF_POINTER) ? *(const void *const *)m == sptr : (const void *)m == sptr) {
                best = e;
                break;
            }
        }
        if(!best) { /* inside a member (an open type's union): the member at the largest offset below it */
            for(unsigned i = 0; i < pt->elements_count; i++) {
                const asn_TYPE_member_t *e = &pt->elements[i];
                if(e->flags & ATF_POINTER) continue;
                if((const char *)sptr >= base + e->memb_offset && (!best || e->memb_offset > best->memb_offset))
                    best = e;
            }
        }
        if(best) {
            snprintf(tmp, sizeof tmp, "%s.%s", p->path, best->name);
            return strdup(tmp);
        }
        break;
    }
    default: break;
    }
    snprintf(tmp, sizeof tmp, "%s.<%s>", p->path, td->name);
    return strdup(tmp);
}

static asn_enc_rval_t shim(const asn_TYPE_descriptor_t *td, const asn_per_constraints_t *ct, const void *sptr,
                           asn_per_outp_t *po, int aper) {
    orig_t *o = find_orig(td->op);
    per_type_encoder_f *real = o ? (aper ? o->aper : o->uper) : NULL;
    if(!real) {
        asn_enc_rval_t bad = {-1, td, sptr};
        fail("an op table the tracer did not patch");
        return bad;
    }
    if(!R.recording) return real(td, ct, sptr, po);
    if(R.depth >= 512) {
        fail("types nested more than 512 deep");
        return real(td, ct, sptr, po);
    }
    if(R.nframes == R.cframes) {
        R.cframes = R.cframes ? R.cframes * 2 : 256;
        R.frames = realloc(R.frames, R.cframes * sizeof *R.frames);
    }
    fr_t *parent = R.depth ? &R.frames[R.stack[R.depth - 1]] : NULL;
    int id = (int)R.nframes++;
    fr_t *f = &R.frames[id];
    memset(f, 0, sizeof *f);
    f->td = td;
    f->sptr = sptr;
    f->ct = ct ? ct : td->encoding_constraints.per_constraints;
    f->po = po;
    f->buf = (parent && parent->po != po) ? new_buffer(po, sptr) : buf_for(po);
    f->parent = parent ? R.stack[R.depth - 1] : -1;
    f->path = child_path(parent, td, sptr);
    f->depth = R.depth;
    f->start = pos_of(po);
    if(parent) parent->nchildren++;
    R.stack[R.depth++] = id;
    asn_enc_rval_t r = real(td, ct, sptr, po);
    R.depth--;
    R.frames[id].end = pos_of(po); /* R.frames may have moved */
    return r;
}

static asn_enc_rval_t shim_uper(const asn_TYPE_descriptor_t *td, const asn_per_constraints_t *ct, const void *sptr,
                                asn_per_outp_t *po) {
    return shim(td, ct, sptr, po, 0);
}
static asn_enc_rval_t shim_aper(const asn_TYPE_descriptor_t *td, const asn_per_constraints_t *ct, const void *sptr,
                                asn_per_outp_t *po) {
    return shim(td, ct, sptr, po, 1);
}

/* What a write is, from the helper that made it or (Tier 3) from the frame. */
static void classify(wr_t *w, fr_t *f, asn_per_outp_t *po) {
    w->source = ASN1VIZ_EXACT;
    w->member = NULL;
    if(!f) {
        w->kind = "structural";
        w->source = ASN1VIZ_INFERRED;
        return;
    }
    const int k = kind_of(f->td);
    if(R.helper) {
        const char *h = R.helper;
        if(!strcmp(h, "align")) {
            w->kind = "alignment";
        } else if(!strcmp(h, "length")) {
            if(R.nots > 0 && !R.ots[R.nots - 1].sorting && po == R.ots[R.nots - 1].outer_po)
                w->kind = "open-type-length";
            else
                w->kind = k == K_LIST ? "count" : "length";
        } else if(!strcmp(h, "nslength")) {
            if(k == K_SEQUENCE) {
                w->kind = "extension-bitmap-length";
                f->saw_nslength = 1;
            } else {
                w->kind = "normally-small-length";
            }
        } else { /* nsnnwn */
            w->kind = k == K_CHOICE ? "choice-index" : "normally-small-number";
        }
        if(k == K_CHOICE && !strcmp(w->kind, "choice-index")) goto choice_member;
        return;
    }
    w->source = ASN1VIZ_INFERRED;
    switch(k) {
    case K_SEQUENCE: {
        const asn_SEQUENCE_specifics_t *ss = f->td->specifics;
        const int ext = ss && ss->first_extension >= 0;
        if(f->nchildren == 0 && !f->saw_nslength) {
            int idx = f->pre_writes++;
            if(ext && idx == 0) {
                w->kind = "extension-bit";
            } else {
                unsigned om = (unsigned)(idx - ext);
                w->kind = "presence-bit";
                if(ss && om < ss->roms_count) w->member = f->td->elements[ss->oms[om]].name;
            }
        } else if(f->saw_nslength) {
            int idx = f->ext_bits++;
            w->kind = "extension-bitmap";
            if(ss && ss->first_extension >= 0 && (unsigned)(ss->first_extension + idx) < f->td->elements_count)
                w->member = f->td->elements[ss->first_extension + idx].name;
        } else {
            w->kind = "structural";
        }
        return;
    }
    case K_CHOICE: {
        const asn_CHOICE_specifics_t *cs = f->td->specifics;
        if(cs && cs->ext_start >= 0 && f->pre_writes++ == 0 && w->n == 1) {
            w->kind = "extension-bit";
            return;
        }
        w->kind = "choice-index";
        goto choice_member;
    }
    case K_LIST: w->kind = "count"; return;
    default: {
        const asn_per_constraints_t *ct = f->ct;
        int idx = f->leaf_writes++;
        if(idx == 0 && w->n == 1 && ct && ((ct->value.flags | ct->size.flags) & APC_EXTENSIBLE)) {
            w->kind = "extension-bit";
            f->leaf_ext = 1;
        } else if(idx == f->leaf_ext && ct && (ct->size.flags & APC_CONSTRAINED) && ct->size.range_bits > 0
                  && (int)w->n == ct->size.range_bits) {
            w->kind = "length";
        } else {
            w->kind = "value";
            w->source = ASN1VIZ_EXACT;
        }
        return;
    }
    }
choice_member : {
    const asn_CHOICE_specifics_t *cs = f->td->specifics;
    const char *base = (const char *)f->sptr + cs->pres_offset;
    unsigned pres = cs->pres_size == 1 ? *(const uint8_t *)base
                    : cs->pres_size == 2 ? *(const uint16_t *)base
                                         : *(const uint32_t *)base;
    if(pres >= 1 && pres <= f->td->elements_count) w->member = f->td->elements[pres - 1].name;
}
}

static void record(asn_bit_outp_t *po, size_t n, uint32_t val, int few, const uint8_t *src) {
    if(!R.recording || n == 0) return;
    if(R.nwrites == R.cwrites) {
        R.cwrites = R.cwrites ? R.cwrites * 2 : 1024;
        R.writes = realloc(R.writes, R.cwrites * sizeof *R.writes);
    }
    wr_t *w = &R.writes[R.nwrites++];
    memset(w, 0, sizeof *w);
    w->buf = buf_for(po);
    w->pos = pos_of(po);
    w->n = n;
    w->val = val;
    w->few = few;
    w->frame = R.depth ? R.stack[R.depth - 1] : -1;
    fr_t *f = w->frame >= 0 ? &R.frames[w->frame] : NULL;
    /* A SET OF element's octets, relayed from its sorting-pass encoding. */
    if(!few && src) {
        for(size_t i = 0; i < R.npending; i++)
            if(R.pending[i].src == src && R.pending[i].buf >= 0) {
                buf_t *b = &R.bufs[R.pending[i].buf];
                b->has_reloc = 1;
                b->reloc = w->pos;
                b->content_bits = n;
                b->content_frame = w->frame;
                R.pending[i].buf = -1;
                w->drop = 1;
                w->kind = "set-of-element";
                return;
            }
    }
    /* An open type's octets in its parent: relocate the open type's buffer here. */
    if(!few && R.nots > 0 && !R.ots[R.nots - 1].sorting && !R.helper && po == R.ots[R.nots - 1].outer_po) {
        ot_t *ot = &R.ots[R.nots - 1];
        if(ot->inner_buf < 0) {
            fail("open type octets written before the open type was encoded");
        } else if(ot->content_writes++) {
            fail("a fragmented open type (16K octets or more): not supported (RFC 0023 section 2.3)");
        } else {
            buf_t *b = &R.bufs[ot->inner_buf];
            b->has_reloc = 1;
            b->reloc = w->pos;
            b->content_bits = n;
            b->content_frame = w->frame;
            w->drop = 1;
            w->kind = "open-type";
            return;
        }
    }
    classify(w, f, po);
}

/* ---- ld --wrap: the bit writers ---- */
int __real_asn_put_few_bits(asn_bit_outp_t *, uint32_t, int);
int __wrap_asn_put_few_bits(asn_bit_outp_t *po, uint32_t bits, int n) {
    if(R.write_depth) return __real_asn_put_few_bits(po, bits, n); /* nested inside a write (wasm-ld wraps these) */
    if(n > 0) record(po, (size_t)n, bits, 1, NULL);
    R.write_depth++;
    int r = __real_asn_put_few_bits(po, bits, n);
    R.write_depth--;
    return r;
}
int __real_asn_put_many_bits(asn_bit_outp_t *, const uint8_t *, int);
int __wrap_asn_put_many_bits(asn_bit_outp_t *po, const uint8_t *src, int n) {
    if(R.write_depth) return __real_asn_put_many_bits(po, src, n);
    if(n > 0) record(po, (size_t)n, 0, 0, src);
    R.write_depth++;
    int r = __real_asn_put_many_bits(po, src, n);
    R.write_depth--;
    return r;
}

/* ---- ld --wrap: labelled helpers ---- */
static void helper_enter(const char *label, asn_per_outp_t *po) {
    if(R.helper_depth++ == 0) {
        R.helper = label;
        R.helper_po = po;
        R.helper_first = R.nwrites;
    }
}
static void helper_leave(void) {
    if(--R.helper_depth) return;
    /* APER lengths align first; GNU ld cannot wrap that call (same translation
     * unit), so a leading zero run that ends on an octet boundary, followed by
     * more of the helper's writes, is the alignment. Inferred. */
    if(R.recording && R.codec == ASN1VIZ_APER && R.nwrites >= R.helper_first + 2) {
        wr_t *w = &R.writes[R.helper_first];
        if(w->few && w->val == 0 && (w->pos & 7) && ((w->pos + w->n) & 7) == 0 && !w->drop) {
            w->kind = "alignment";
            w->source = ASN1VIZ_INFERRED;
        }
    }
    R.helper = NULL;
    R.helper_po = NULL;
}

ssize_t __real_uper_put_length(asn_per_outp_t *, size_t, int *);
ssize_t __wrap_uper_put_length(asn_per_outp_t *po, size_t l, int *e) {
    helper_enter("length", po);
    ssize_t r = __real_uper_put_length(po, l, e);
    helper_leave();
    return r;
}
int __real_uper_put_nslength(asn_per_outp_t *, size_t);
int __wrap_uper_put_nslength(asn_per_outp_t *po, size_t l) {
    helper_enter("nslength", po);
    int r = __real_uper_put_nslength(po, l);
    helper_leave();
    return r;
}
int __real_uper_put_nsnnwn(asn_per_outp_t *, int);
int __wrap_uper_put_nsnnwn(asn_per_outp_t *po, int n) {
    helper_enter("nsnnwn", po);
    int r = __real_uper_put_nsnnwn(po, n);
    helper_leave();
    return r;
}
ssize_t __real_aper_put_length(asn_per_outp_t *, ssize_t, ssize_t, size_t, int *);
ssize_t __wrap_aper_put_length(asn_per_outp_t *po, ssize_t lb, ssize_t ub, size_t n, int *e) {
    helper_enter("length", po);
    ssize_t r = __real_aper_put_length(po, lb, ub, n, e);
    helper_leave();
    return r;
}
int __real_aper_put_nslength(asn_per_outp_t *, size_t);
int __wrap_aper_put_nslength(asn_per_outp_t *po, size_t l) {
    helper_enter("nslength", po);
    int r = __real_aper_put_nslength(po, l);
    helper_leave();
    return r;
}
int __real_aper_put_nsnnwn(asn_per_outp_t *, int);
int __wrap_aper_put_nsnnwn(asn_per_outp_t *po, int n) {
    helper_enter("nsnnwn", po);
    int r = __real_aper_put_nsnnwn(po, n);
    helper_leave();
    return r;
}
int __real_aper_put_align(asn_per_outp_t *);
int __wrap_aper_put_align(asn_per_outp_t *po) {
    helper_enter("align", po);
    int r = __real_aper_put_align(po);
    helper_leave();
    return r;
}

/* ---- ld --wrap: open types ---- */
static void ot_enter(asn_per_outp_t *po, int sorting) {
    if(!R.recording) return;
    if(R.nots >= 64) {
        fail("open types nested more than 64 deep");
        return;
    }
    ot_t *ot = &R.ots[R.nots++];
    ot->sorting = sorting;
    ot->outer_po = po;
    ot->outer_buf = buf_for(po);
    ot->inner_buf = -1;
    ot->map_mark = R.nmap;
    ot->content_writes = 0;
    /* The helper label (if any) belongs to the parent, not to the inner encode. */
}
static void ot_leave(void) {
    if(!R.recording || R.nots == 0) return;
    ot_t *ot = &R.ots[--R.nots];
    R.nmap = ot->map_mark; /* the inner stream's address may be reused by the next one */
    if(!ot->sorting && ot->inner_buf >= 0 && !R.bufs[ot->inner_buf].has_reloc)
        fail("an open type whose octets were never written");
}

int __real_uper_open_type_put(const asn_TYPE_descriptor_t *, const asn_per_constraints_t *, const void *,
                              asn_per_outp_t *);
int __wrap_uper_open_type_put(const asn_TYPE_descriptor_t *td, const asn_per_constraints_t *ct, const void *sptr,
                              asn_per_outp_t *po) {
    ot_enter(po, 0);
    int r = __real_uper_open_type_put(td, ct, sptr, po);
    ot_leave();
    return r;
}
int __real_aper_open_type_put(const asn_TYPE_descriptor_t *, const asn_per_constraints_t *, const void *,
                              asn_per_outp_t *);
int __wrap_aper_open_type_put(const asn_TYPE_descriptor_t *td, const asn_per_constraints_t *ct, const void *sptr,
                              asn_per_outp_t *po) {
    ot_enter(po, 0);
    int r = __real_aper_open_type_put(td, ct, sptr, po);
    ot_leave();
    return r;
}

/* ---- ld --wrap: SET OF sorting (X.691 22.1) ----
 * asn1c encodes every SET OF element on its own to sort them. UPER then
 * relays those octets (exact bit counts) in sorted order; APER encodes each
 * element again in place, so its sorting-pass buffers are never used. */
struct _el_buffer *__real_SET_OF__encode_sorted(const asn_TYPE_member_t *, const asn_anonymous_set_ *,
                                                enum SET_OF__encode_method);
struct _el_buffer *__wrap_SET_OF__encode_sorted(const asn_TYPE_member_t *elm, const asn_anonymous_set_ *list,
                                                enum SET_OF__encode_method method) {
    if(!R.recording || method == SOES_DER) return __real_SET_OF__encode_sorted(elm, list, method);
    asn_per_outp_t *outer = R.depth ? R.frames[R.stack[R.depth - 1]].po : NULL;
    int first = R.nbufs;
    ot_enter(outer, 1);
    struct _el_buffer *els = __real_SET_OF__encode_sorted(elm, list, method);
    ot_leave();
    for(int i = 0; method == SOES_CUPER && els && i < list->count; i++) {
        for(int b = first; b < R.nbufs; b++) {
            if(R.bufs[b].sorted && R.bufs[b].root_sptr == els[i].memb_ptr && R.bufs[b].parent_buf == buf_for(outer)) {
                if(R.npending < 1024) {
                    R.pending[R.npending].src = els[i].buf;
                    R.pending[R.npending].buf = b;
                    R.npending++;
                } else {
                    fail("more than 1024 SET OF elements");
                }
                break;
            }
        }
    }
    return els;
}

/* ------------------------------------------------------------------ encode + assemble */

typedef struct {
    uint8_t *data;
    size_t len, cap;
} dyn_t;

static int dyn_cb(const void *buf, size_t size, void *key) {
    dyn_t *d = key;
    if(d->len + size > d->cap) {
        d->cap = (d->len + size) * 2 + 64;
        d->data = realloc(d->data, d->cap);
        if(!d->data) return -1;
    }
    memcpy(d->data + d->len, buf, size);
    d->len += size;
    return 0;
}

static asn_enc_rval_t encode(asn_TYPE_descriptor_t *pdu, const void *value, int codec, dyn_t *out) {
    memset(out, 0, sizeof *out);
    return codec == ASN1VIZ_APER ? aper_encode(pdu, NULL, value, dyn_cb, out)
                                 : uper_encode(pdu, NULL, value, dyn_cb, out);
}

static size_t abs_pos(int buf, size_t p) {
    while(buf > 0) {
        p += R.bufs[buf].reloc;
        buf = R.bufs[buf].parent_buf;
    }
    return p;
}

static int seg_cmp(const void *a, const void *b) {
    const asn1viz_segment_t *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start ? 1 : 0;
}

static int mergeable(const char *kind) {
    return !strcmp(kind, "value") || !strcmp(kind, "alignment") || !strcmp(kind, "open-type-padding")
           || !strcmp(kind, "trailing-padding");
}

static int same_str(const char *a, const char *b) { return a == b || (a && b && !strcmp(a, b)); }

static void reset_recording(int codec) {
    for(size_t i = 0; i < R.nframes; i++) free(R.frames[i].path);
    free(R.frames);
    free(R.writes);
    memset(&R, 0, sizeof R);
    R.codec = codec;
}

int asn1viz_trace_encode(asn_TYPE_descriptor_t *pdu, const void *value, int codec, asn1viz_lane_t *lane) {
    memset(lane, 0, sizeof *lane);
    lane->codec = codec;

    /* Untraced: the original tables. */
    int was = g_installed;
    asn1viz_trace_restore();
    dyn_t plain;
    asn_enc_rval_t er0 = encode(pdu, value, codec, &plain);
    asn1viz_trace_install(was ? g_pdu : pdu);
    if(er0.encoded < 0) {
        snprintf(lane->error, sizeof lane->error, "encoding failed (untraced) at %s",
                 er0.failed_type ? er0.failed_type->name : "?");
        free(plain.data);
        return -1;
    }

    /* Traced. */
    reset_recording(codec);
    R.recording = 1;
    dyn_t traced;
    asn_enc_rval_t er = encode(pdu, value, codec, &traced);
    R.recording = 0;
    if(er.encoded < 0) {
        snprintf(lane->error, sizeof lane->error, "encoding failed (traced) at %s",
                 er.failed_type ? er.failed_type->name : "?");
        free(plain.data);
        free(traced.data);
        return -1;
    }
    lane->encoded_bits = (size_t)er.encoded;
    lane->nbytes = traced.len;
    lane->bytes = traced.data;
    lane->bits = traced.len * 8;
    lane->matches_untraced = plain.len == traced.len && !memcmp(plain.data, traced.data, plain.len);
    free(plain.data);

    /* Buffers whose bits reach the output: the root, and every inner buffer
     * relocated into a live parent. APER's SET OF sorting-pass buffers are not. */
    int *alive = calloc((size_t)R.nbufs + 1, sizeof *alive);
    for(int b = 0; b < R.nbufs; b++) {
        if(b == 0) alive[b] = 1;
        else alive[b] = R.bufs[b].has_reloc && R.bufs[b].parent_buf >= 0 && alive[R.bufs[b].parent_buf];
        if(b > 0 && !R.bufs[b].has_reloc && !R.bufs[b].sorted && !R.error[0])
            snprintf(R.error, sizeof R.error, "an inner encoding whose bits never reached the output");
    }

    /* Frames, in absolute bits, renumbered without the unused ones. */
    int *remap = malloc((R.nframes ? R.nframes : 1) * sizeof *remap);
    lane->frames = calloc(R.nframes ? R.nframes : 1, sizeof *lane->frames);
    size_t nf = 0;
    for(size_t i = 0; i < R.nframes; i++) {
        fr_t *f = &R.frames[i];
        if(!alive[f->buf]) {
            remap[i] = -1;
            continue;
        }
        remap[i] = (int)nf;
        asn1viz_frame_t *o = &lane->frames[nf++];
        o->id = remap[i];
        o->parent = f->parent >= 0 ? remap[f->parent] : -1;
        o->path = f->path;
        f->path = NULL;
        o->type = f->td->name;
        o->start = abs_pos(f->buf, f->start);
        o->end = abs_pos(f->buf, f->end);
        o->depth = f->depth;
    }
    lane->nframes = nf;
    lane->root_span_ok = nf > 0 && lane->frames[0].start == 0 && lane->frames[0].end == lane->encoded_bits;

    /* Segments: every write, relocated; each open type's own padding; the trailing fill. */
    size_t cap = R.nwrites + (size_t)R.nbufs + 2, n = 0;
    asn1viz_segment_t *segs = calloc(cap, sizeof *segs);
    for(size_t i = 0; i < R.nwrites; i++) {
        wr_t *w = &R.writes[i];
        if(w->drop || !alive[w->buf]) continue;
        segs[n].start = abs_pos(w->buf, w->pos);
        segs[n].len = w->n;
        segs[n].frame = w->frame >= 0 ? remap[w->frame] : 0;
        segs[n].kind = w->kind;
        segs[n].source = w->source;
        segs[n].member = w->member;
        n++;
    }
    for(int b = 1; b < R.nbufs; b++) {
        if(!alive[b]) continue;
        size_t used = 0;
        for(size_t i = 0; i < R.nwrites; i++)
            if(R.writes[i].buf == b && !R.writes[i].drop && R.writes[i].pos + R.writes[i].n > used)
                used = R.writes[i].pos + R.writes[i].n;
        if(R.bufs[b].content_bits > used) {
            segs[n].start = abs_pos(b, used);
            segs[n].len = R.bufs[b].content_bits - used;
            segs[n].frame = R.bufs[b].content_frame >= 0 ? remap[R.bufs[b].content_frame] : 0;
            segs[n].kind = "open-type-padding";
            segs[n].source = ASN1VIZ_EXACT;
            n++;
        }
    }
    if(lane->bits > lane->encoded_bits) {
        segs[n].start = lane->encoded_bits;
        segs[n].len = lane->bits - lane->encoded_bits;
        segs[n].frame = 0;
        segs[n].kind = "trailing-padding";
        segs[n].source = ASN1VIZ_EXACT;
        n++;
    }
    free(alive);
    free(remap);
    qsort(segs, n, sizeof *segs, seg_cmp);

    /* Self-test: the segments tile [0, bits) exactly. */
    size_t at = 0;
    lane->tiles = 1;
    for(size_t i = 0; i < n; i++) {
        if(segs[i].start != at) {
            lane->tiles = 0;
            if(!R.error[0])
                snprintf(R.error, sizeof R.error, "segments do not tile: %s at bit %zu (expected %zu)",
                         segs[i].start > at ? "gap" : "overlap", segs[i].start, at);
            break;
        }
        at = segs[i].start + segs[i].len;
    }
    if(lane->tiles && at != lane->bits) {
        lane->tiles = 0;
        if(!R.error[0]) snprintf(R.error, sizeof R.error, "segments end at bit %zu, not %zu", at, lane->bits);
    }

    /* Merge runs of the same thing (a string's characters, a padding run). */
    size_t m = 0;
    for(size_t i = 0; i < n; i++) {
        if(m > 0) {
            asn1viz_segment_t *p = &segs[m - 1];
            if(p->frame == segs[i].frame && p->kind == segs[i].kind && p->source == segs[i].source
               && same_str(p->member, segs[i].member) && mergeable(p->kind) && p->start + p->len == segs[i].start) {
                p->len += segs[i].len;
                continue;
            }
        }
        segs[m++] = segs[i];
    }
    lane->segments = segs;
    lane->nsegments = m;

    if(R.error[0]) snprintf(lane->error, sizeof lane->error, "%s", R.error);
    else if(!lane->matches_untraced) snprintf(lane->error, sizeof lane->error, "traced bytes differ from untraced bytes");
    else if(!lane->root_span_ok)
        snprintf(lane->error, sizeof lane->error, "the root frame does not span [0, %zu)", lane->encoded_bits);
    return lane->error[0] ? -1 : 0;
}

void asn1viz_lane_free(asn1viz_lane_t *lane) {
    for(size_t i = 0; i < lane->nframes; i++) free(lane->frames[i].path);
    free(lane->frames);
    free(lane->segments);
    free(lane->bytes);
    memset(lane, 0, sizeof *lane);
}

/* ------------------------------------------------------------------ JSON */

void asn1viz_json_string(FILE *out, const char *s) {
    fputc('"', out);
    for(; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if(c == '"' || c == '\\') fprintf(out, "\\%c", c);
        else if(c < 0x20) fprintf(out, "\\u%04x", c);
        else fputc(c, out);
    }
    fputc('"', out);
}

void asn1viz_lane_write_json(FILE *out, const asn1viz_lane_t *l, const char *ind) {
    fprintf(out, "{\n%s  \"codec\": \"%s\",\n", ind, l->codec == ASN1VIZ_APER ? "aper" : "uper");
    fprintf(out, "%s  \"bits\": %zu,\n%s  \"encodedBits\": %zu,\n", ind, l->bits, ind, l->encoded_bits);
    fprintf(out, "%s  \"hex\": \"", ind);
    for(size_t i = 0; i < l->nbytes; i++) fprintf(out, "%02x", l->bytes[i]);
    fprintf(out, "\",\n%s  \"selfTest\": { \"tiles\": %s, \"matchesUntraced\": %s, \"rootSpan\": %s },\n", ind,
            l->tiles ? "true" : "false", l->matches_untraced ? "true" : "false", l->root_span_ok ? "true" : "false");
    fprintf(out, "%s  \"frames\": [\n", ind);
    for(size_t i = 0; i < l->nframes; i++) {
        const asn1viz_frame_t *f = &l->frames[i];
        fprintf(out, "%s    { \"id\": %d, \"parent\": %d, \"depth\": %d, \"path\": ", ind, f->id, f->parent, f->depth);
        asn1viz_json_string(out, f->path);
        fprintf(out, ", \"type\": ");
        asn1viz_json_string(out, f->type);
        fprintf(out, ", \"start\": %zu, \"end\": %zu }%s\n", f->start, f->end, i + 1 < l->nframes ? "," : "");
    }
    fprintf(out, "%s  ],\n%s  \"segments\": [\n", ind, ind);
    for(size_t i = 0; i < l->nsegments; i++) {
        const asn1viz_segment_t *s = &l->segments[i];
        fprintf(out, "%s    { \"start\": %zu, \"len\": %zu, \"frame\": %d, \"kind\": \"%s\", \"source\": \"%s\"", ind,
                s->start, s->len, s->frame, s->kind, s->source == ASN1VIZ_INFERRED ? "inferred" : "exact");
        if(s->member) {
            fprintf(out, ", \"member\": ");
            asn1viz_json_string(out, s->member);
        }
        fprintf(out, " }%s\n", i + 1 < l->nsegments ? "," : "");
    }
    fprintf(out, "%s  ],\n%s  \"canonical\": null\n%s}", ind, ind, ind);
}
