/* The asn1-viz tracer. See trace.h and RFC 0023 section 2. */
#include "trace.h"

#include <stdlib.h>
#include <string.h>

#include "aper_encoder.h"
#include "aper_support.h"
#include "asn_SET_OF.h"
#include "asn_bit_data.h"
#include "constr_CHOICE.h"
#include "constr_SEQUENCE.h"
#include "constr_SET_OF.h"
#include "per_support.h"
#include "uper_decoder.h"
#include "uper_encoder.h"
#include "uper_support.h"

/* The constructed types' op tables, compared by address to classify a frame.
 * Weak, so that a table a schema does not link in reads as absent. */
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
    per_type_decoder_f *udec;
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
static asn_dec_rval_t shim_udec(const asn_codec_ctx_t *, const asn_TYPE_descriptor_t *,
                                const asn_per_constraints_t *, void **, asn_per_data_t *);

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
        o->udec = td->op->uper_decoder;
        if(o->uper) td->op->uper_encoder = shim_uper;
        if(o->aper) td->op->aper_encoder = shim_aper;
        if(o->udec) td->op->uper_decoder = shim_udec;
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
        g_origs[i].op->uper_decoder = g_origs[i].udec;
    }
    g_norigs = 0;
    g_installed = 0;
}

/* ------------------------------------------------------------------ recording */

typedef struct {
    const asn_TYPE_descriptor_t *td;
    const void *sptr;   /* encode: the value */
    void **sptrp;       /* decode: where the decoder puts the value */
    const asn_per_constraints_t *ct; /* effective PER constraints */
    const void *stream; /* asn_per_outp_t (encode) or asn_per_data_t (decode) */
    int buf;
    int parent;
    int index;          /* its place among its parent's children */
    size_t start, end;
    char *path;         /* encode: the full path; decode: resolved at the end */
    char *comp;         /* decode: ".member", "[i]", or the root's name */
    int depth;
    int nchildren;
    int code;           /* decode: the decoder's result (RC_OK, ...); -1 while open */
    int pre_writes;     /* unlabelled writes before the first member (SEQUENCE/CHOICE preamble) */
    int saw_nslength;   /* SEQUENCE: the extension bitmap's length was written */
    int ext_bits;       /* SEQUENCE: extension bitmap bits written so far */
    int leaf_writes;    /* leaf: unlabelled writes so far */
    int leaf_ext;       /* leaf: its first write was an extension bit */
} fr_t;

typedef struct {
    int buf;
    size_t pos;
    size_t n;
    int frame;
    const char *kind;
    int source;
    const char *member;
    int drop;      /* an open type's or SET OF element's octets, replaced by their own segments; a failed read */
    uint32_t val;  /* few-bit writes: the bits written */
    int few;
} wr_t;

typedef struct {
    int parent_buf;
    int sorted;          /* a SET OF element encoded for sorting: used only if its octets are relayed */
    const void *root_sptr;
    int has_reloc;
    size_t reloc;        /* where bit 0 of this buffer lands in the parent buffer */
    size_t content_bits; /* bits carried in the parent */
    int content_frame;   /* the parent frame that carried them */
} buf_t;

/* A separate-buffer context: an open type (one inner encode or decode) or a
 * SET OF sorting pass (one inner encode per element). */
typedef struct {
    int sorting;
    const void *outer;
    int outer_buf;
    int inner_buf; /* open type: -1 until the inner encode/decode starts */
    size_t map_mark;
    int has_content;
    size_t content_write; /* the write that carried the octets */
} ot_t;

typedef struct {
    const void *stream;
    int buf;
} map_t;

/* SET OF elements awaiting their octets: the element buffer asn1c will
 * relay, and the tracer buffer that recorded its encoding. */
typedef struct {
    const uint8_t *src;
    int buf;
} pending_t;

static struct {
    int recording;
    int decoding;
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
    size_t helper_first; /* index of its first write */
    int io_depth;        /* inside a wrapped bit writer/reader */
    char error[256];
} R;

static void fail(const char *msg) {
    if(!R.error[0]) snprintf(R.error, sizeof R.error, "%s", msg);
}

static size_t stream_pos(const void *s) {
    return R.decoding ? ((const asn_per_data_t *)s)->moved : pos_of((const asn_per_outp_t *)s);
}

static const void *frame_struct(const fr_t *f) {
    if(!R.decoding) return f->sptr;
    return f->sptrp ? *f->sptrp : NULL;
}

static void map_add(const void *s, int b) {
    if(R.nmap < 256) {
        R.map[R.nmap].stream = s;
        R.map[R.nmap].buf = b;
        R.nmap++;
    } else {
        fail("too many streams at once");
    }
}

/* The buffer a stream belongs to: the latest one opened for it. */
static int buf_for(const void *s) {
    for(size_t i = R.nmap; i-- > 0;)
        if(R.map[i].stream == s) return R.map[i].buf;
    if(R.nbufs == 0) { /* the root stream */
        R.bufs[0].parent_buf = -1;
        R.nbufs = 1;
        map_add(s, 0);
        return 0;
    }
    char msg[256];
    snprintf(msg, sizeof msg, "bits on a stream the tracer cannot place (not the root, an open type or a SET OF), inside %s",
             R.depth ? (R.frames[R.stack[R.depth - 1]].path ? R.frames[R.stack[R.depth - 1]].path : R.frames[R.stack[R.depth - 1]].td->name) : "(no frame)");
    fail(msg);
    return 0;
}

/* A new inner encode or decode on its own stream, inside an open type or a
 * SET OF sorting pass (a stream's address can be reused by the next one, so
 * every inner pass gets its own buffer). */
static int new_buffer(const void *s, const void *root_sptr) {
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
    map_add(s, b);
    return b;
}

/* A child's name relative to its parent: ".member", "[i]", or "<Type>". */
static char *component(const fr_t *p, const asn_TYPE_descriptor_t *td, const void *child, int index) {
    char tmp[512];
    if(!p) return strdup(td->name);
    const asn_TYPE_descriptor_t *pt = p->td;
    const void *pstruct = frame_struct(p);
    switch(kind_of(pt)) {
    case K_CHOICE: {
        const asn_CHOICE_specifics_t *cs = pt->specifics;
        if(!pstruct) break;
        const char *base = (const char *)pstruct + cs->pres_offset;
        unsigned pres = cs->pres_size == 1 ? *(const uint8_t *)base
                        : cs->pres_size == 2 ? *(const uint16_t *)base
                                             : *(const uint32_t *)base;
        if(pres >= 1 && pres <= pt->elements_count) {
            snprintf(tmp, sizeof tmp, ".%s", pt->elements[pres - 1].name);
            return strdup(tmp);
        }
        break;
    }
    case K_LIST: {
        int idx = index;
        if(!R.decoding && pstruct) {
            /* The element's index in the value (SET OF elements are written in sorted order). */
            const asn_anonymous_set_ *list = _A_CSET_FROM_VOID(pstruct);
            for(int i = 0; i < list->count; i++)
                if(list->array[i] == child) {
                    idx = i;
                    break;
                }
        }
        snprintf(tmp, sizeof tmp, "[%d]", idx);
        return strdup(tmp);
    }
    case K_SEQUENCE: {
        const char *base = pstruct;
        const asn_TYPE_member_t *best = NULL;
        if(!base || !child) break;
        for(unsigned i = 0; i < pt->elements_count; i++) {
            const asn_TYPE_member_t *e = &pt->elements[i];
            const char *m = base + e->memb_offset;
            if((e->flags & ATF_POINTER) ? *(const void *const *)m == child : (const void *)m == child) {
                best = e;
                break;
            }
        }
        if(!best) { /* inside a member (an open type's union): the member at the largest offset below it */
            for(unsigned i = 0; i < pt->elements_count; i++) {
                const asn_TYPE_member_t *e = &pt->elements[i];
                if(e->flags & ATF_POINTER) continue;
                if((const char *)child >= base + e->memb_offset && (!best || e->memb_offset > best->memb_offset))
                    best = e;
            }
        }
        if(best) {
            snprintf(tmp, sizeof tmp, ".%s", best->name);
            return strdup(tmp);
        }
        break;
    }
    default: break;
    }
    snprintf(tmp, sizeof tmp, ".<%s>", td->name);
    return strdup(tmp);
}

static int frame_enter(const asn_TYPE_descriptor_t *td, const asn_per_constraints_t *ct, const void *stream,
                       const void *sptr, void **sptrp) {
    if(R.depth >= 512) {
        fail("types nested more than 512 deep");
        return -1;
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
    f->sptrp = sptrp;
    f->ct = ct ? ct : td->encoding_constraints.per_constraints;
    f->stream = stream;
    f->buf = (parent && parent->stream != stream) ? new_buffer(stream, sptr) : buf_for(stream);
    f->parent = parent ? R.stack[R.depth - 1] : -1;
    f->index = parent ? parent->nchildren : 0;
    f->depth = R.depth;
    f->code = -1;
    f->start = stream_pos(stream);
    if(!R.decoding) {
        char *c = component(parent, td, sptr, f->index);
        if(parent) {
            size_t n = strlen(parent->path) + strlen(c) + 1;
            f->path = malloc(n);
            snprintf(f->path, n, "%s%s", parent->path, c);
            free(c);
        } else {
            f->path = c;
        }
    }
    if(parent) parent->nchildren++;
    R.stack[R.depth++] = id;
    return id;
}

static void frame_leave(int id, int code) {
    R.depth--;
    fr_t *f = &R.frames[id]; /* R.frames may have moved during the call */
    f->end = stream_pos(f->stream);
    f->code = code;
    if(R.decoding) {
        fr_t *parent = f->parent >= 0 ? &R.frames[f->parent] : NULL;
        f->comp = component(parent, f->td, f->sptrp ? *f->sptrp : NULL, f->index);
    }
}

static asn_enc_rval_t shim_enc(const asn_TYPE_descriptor_t *td, const asn_per_constraints_t *ct, const void *sptr,
                               asn_per_outp_t *po, int aper) {
    orig_t *o = find_orig(td->op);
    per_type_encoder_f *real = o ? (aper ? o->aper : o->uper) : NULL;
    if(!real) {
        asn_enc_rval_t bad = {-1, td, sptr};
        fail("an op table the tracer did not patch");
        return bad;
    }
    if(!R.recording || R.decoding) return real(td, ct, sptr, po);
    int id = frame_enter(td, ct, po, sptr, NULL);
    if(id < 0) return real(td, ct, sptr, po);
    asn_enc_rval_t r = real(td, ct, sptr, po);
    frame_leave(id, r.encoded < 0 ? RC_FAIL : RC_OK);
    return r;
}

static asn_enc_rval_t shim_uper(const asn_TYPE_descriptor_t *td, const asn_per_constraints_t *ct, const void *sptr,
                                asn_per_outp_t *po) {
    return shim_enc(td, ct, sptr, po, 0);
}
static asn_enc_rval_t shim_aper(const asn_TYPE_descriptor_t *td, const asn_per_constraints_t *ct, const void *sptr,
                                asn_per_outp_t *po) {
    return shim_enc(td, ct, sptr, po, 1);
}

static asn_dec_rval_t shim_udec(const asn_codec_ctx_t *ctx, const asn_TYPE_descriptor_t *td,
                                const asn_per_constraints_t *ct, void **sptr, asn_per_data_t *pd) {
    orig_t *o = find_orig(td->op);
    if(!o || !o->udec) {
        asn_dec_rval_t bad = {RC_FAIL, 0};
        fail("an op table the tracer did not patch");
        return bad;
    }
    if(!R.recording || !R.decoding) return o->udec(ctx, td, ct, sptr, pd);
    int id = frame_enter(td, ct, pd, NULL, sptr);
    if(id < 0) return o->udec(ctx, td, ct, sptr, pd);
    asn_dec_rval_t r = o->udec(ctx, td, ct, sptr, pd);
    frame_leave(id, r.code);
    return r;
}

/* What a write (or read) is, from the helper that made it or (Tier 3) from
 * the frame. */
static void classify(wr_t *w, fr_t *f, const void *stream) {
    w->source = ASN1VIZ_EXACT;
    w->member = NULL;
    if(!f) {
        w->kind = "structural";
        w->source = ASN1VIZ_INFERRED;
        return;
    }
    const int k = kind_of(f->td);
    const void *st = frame_struct(f);
    if(R.helper) {
        const char *h = R.helper;
        if(!strcmp(h, "align")) {
            w->kind = "alignment";
        } else if(!strcmp(h, "length")) {
            if(R.nots > 0 && !R.ots[R.nots - 1].sorting && stream == R.ots[R.nots - 1].outer)
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
            if(ext && idx == 0 && w->n == 1) {
                w->kind = "extension-bit";
            } else if(w->n > 1) {
                w->kind = "presence-bitmap"; /* the decoder reads the bitmap in one go */
            } else {
                unsigned om = (unsigned)(idx - ext);
                w->kind = "presence-bit";
                if(ss && om < ss->roms_count) w->member = f->td->elements[ss->oms[om]].name;
            }
        } else if(f->saw_nslength) {
            int idx = f->ext_bits;
            f->ext_bits += (int)w->n;
            w->kind = "extension-bitmap";
            if(w->n == 1 && ss && ss->first_extension >= 0
               && (unsigned)(ss->first_extension + idx) < f->td->elements_count)
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
    if(!st) return; /* decoding: the alternative is known only later (filled in at the end) */
    const char *base = (const char *)st + cs->pres_offset;
    unsigned pres = cs->pres_size == 1 ? *(const uint8_t *)base
                    : cs->pres_size == 2 ? *(const uint16_t *)base
                                         : *(const uint32_t *)base;
    if(pres >= 1 && pres <= f->td->elements_count) w->member = f->td->elements[pres - 1].name;
}
}

/* Records one write or read at `pos` and returns its index, or -1. */
static long record(const void *stream, size_t pos, size_t n, uint32_t val, int few, const uint8_t *src) {
    if(!R.recording || n == 0) return -1;
    if(R.decoding && R.nbufs > 0) {
        /* The SEQUENCE decoder copies its presence and extension bitmaps
         * into small streams of their own and reads them bit by bit; those
         * bits were already recorded as one read from the real stream. */
        int known = 0;
        for(size_t i = R.nmap; i-- > 0;)
            if(R.map[i].stream == stream) {
                known = 1;
                break;
            }
        if(!known) return -1;
    }
    if(R.nwrites == R.cwrites) {
        R.cwrites = R.cwrites ? R.cwrites * 2 : 1024;
        R.writes = realloc(R.writes, R.cwrites * sizeof *R.writes);
    }
    long idx = (long)R.nwrites;
    wr_t *w = &R.writes[R.nwrites++];
    memset(w, 0, sizeof *w);
    w->buf = buf_for(stream);
    w->pos = pos;
    w->n = n;
    w->val = val;
    w->few = few;
    w->frame = R.depth ? R.stack[R.depth - 1] : -1;
    fr_t *f = w->frame >= 0 ? &R.frames[w->frame] : NULL;
    /* A SET OF element's octets, relayed from its sorting-pass encoding. */
    if(!few && src && !R.decoding) {
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
                return idx;
            }
    }
    /* An open type's octets in its parent: the open type's buffer goes here. */
    if(!few && R.nots > 0 && !R.ots[R.nots - 1].sorting && !R.helper && stream == R.ots[R.nots - 1].outer) {
        ot_t *ot = &R.ots[R.nots - 1];
        if(ot->has_content) {
            fail("a fragmented open type (16K octets or more): not supported (RFC 0023 section 2.3)");
        } else {
            ot->has_content = 1;
            ot->content_write = (size_t)idx;
            w->drop = 1;
            w->kind = "open-type";
            w->source = ASN1VIZ_EXACT;
            return idx;
        }
    }
    classify(w, f, stream);
    return idx;
}

/* ---- ld --wrap: the bit writers (encode) ---- */
int __real_asn_put_few_bits(asn_bit_outp_t *, uint32_t, int);
int __wrap_asn_put_few_bits(asn_bit_outp_t *po, uint32_t bits, int n) {
    if(R.io_depth || R.decoding) return __real_asn_put_few_bits(po, bits, n);
    if(n > 0) record(po, pos_of(po), (size_t)n, bits, 1, NULL);
    R.io_depth++;
    int r = __real_asn_put_few_bits(po, bits, n);
    R.io_depth--;
    return r;
}
int __real_asn_put_many_bits(asn_bit_outp_t *, const uint8_t *, int);
int __wrap_asn_put_many_bits(asn_bit_outp_t *po, const uint8_t *src, int n) {
    if(R.io_depth || R.decoding) return __real_asn_put_many_bits(po, src, n);
    if(n > 0) record(po, pos_of(po), (size_t)n, 0, 0, src);
    R.io_depth++;
    int r = __real_asn_put_many_bits(po, src, n);
    R.io_depth--;
    return r;
}

/* ---- ld --wrap: the bit readers (decode) ---- */
int32_t __real_asn_get_few_bits(asn_bit_data_t *, int);
int32_t __wrap_asn_get_few_bits(asn_bit_data_t *pd, int n) {
    if(R.io_depth || !R.decoding || !R.recording) return __real_asn_get_few_bits(pd, n);
    long w = n > 0 ? record(pd, pd->moved, (size_t)n, 0, 1, NULL) : -1;
    R.io_depth++;
    int32_t r = __real_asn_get_few_bits(pd, n);
    R.io_depth--;
    if(w >= 0) {
        if(r < 0) R.writes[w].drop = 2; /* ran out of bits: nothing was read */
        else R.writes[w].val = (uint32_t)r;
    }
    return r;
}
int __real_asn_get_many_bits(asn_bit_data_t *, uint8_t *, int, int);
int __wrap_asn_get_many_bits(asn_bit_data_t *pd, uint8_t *dst, int right_align, int n) {
    if(R.io_depth || !R.decoding || !R.recording) return __real_asn_get_many_bits(pd, dst, right_align, n);
    long w = n > 0 ? record(pd, pd->moved, (size_t)n, 0, 0, NULL) : -1;
    R.io_depth++;
    int r = __real_asn_get_many_bits(pd, dst, right_align, n);
    R.io_depth--;
    if(w >= 0 && r < 0) R.writes[w].drop = 2;
    return r;
}

/* ---- ld --wrap: labelled helpers ---- */
static void helper_enter(const char *label) {
    if(R.helper_depth++ == 0) {
        R.helper = label;
        R.helper_first = R.nwrites;
    }
}
static void helper_leave(void) {
    if(--R.helper_depth) return;
    /* APER lengths align first; GNU ld cannot wrap that call (same translation
     * unit), so a leading zero run that ends on an octet boundary, followed by
     * more of the helper's writes, is the alignment. Inferred. */
    if(R.recording && !R.decoding && R.codec == ASN1VIZ_APER && R.nwrites >= R.helper_first + 2) {
        wr_t *w = &R.writes[R.helper_first];
        if(w->few && w->val == 0 && (w->pos & 7) && ((w->pos + w->n) & 7) == 0 && !w->drop) {
            w->kind = "alignment";
            w->source = ASN1VIZ_INFERRED;
        }
    }
    R.helper = NULL;
}

#define WRAP_HELPER(ret, fn, params, args, label) \
    ret __real_##fn params;                       \
    ret __wrap_##fn params {                      \
        helper_enter(label);                      \
        ret r = __real_##fn args;                 \
        helper_leave();                           \
        return r;                                 \
    }

WRAP_HELPER(ssize_t, uper_put_length, (asn_per_outp_t * po, size_t l, int *e), (po, l, e), "length")
WRAP_HELPER(int, uper_put_nslength, (asn_per_outp_t * po, size_t l), (po, l), "nslength")
WRAP_HELPER(int, uper_put_nsnnwn, (asn_per_outp_t * po, int n), (po, n), "nsnnwn")
WRAP_HELPER(ssize_t, aper_put_length, (asn_per_outp_t * po, ssize_t lb, ssize_t ub, size_t n, int *e),
            (po, lb, ub, n, e), "length")
WRAP_HELPER(int, aper_put_nslength, (asn_per_outp_t * po, size_t l), (po, l), "nslength")
WRAP_HELPER(int, aper_put_nsnnwn, (asn_per_outp_t * po, int n), (po, n), "nsnnwn")
WRAP_HELPER(int, aper_put_align, (asn_per_outp_t * po), (po), "align")
WRAP_HELPER(ssize_t, uper_get_length, (asn_per_data_t * pd, int ebits, size_t lb, int *repeat),
            (pd, ebits, lb, repeat), "length")
WRAP_HELPER(ssize_t, uper_get_nslength, (asn_per_data_t * pd), (pd), "nslength")
WRAP_HELPER(ssize_t, uper_get_nsnnwn, (asn_per_data_t * pd), (pd), "nsnnwn")

/* ---- ld --wrap: open types ---- */
static void ot_enter(const void *outer, int sorting) {
    if(!R.recording) return;
    if(R.nots >= 64) {
        fail("open types nested more than 64 deep");
        return;
    }
    ot_t *ot = &R.ots[R.nots++];
    memset(ot, 0, sizeof *ot);
    ot->sorting = sorting;
    ot->outer = outer;
    ot->outer_buf = buf_for(outer);
    ot->inner_buf = -1;
    ot->map_mark = R.nmap;
}
static void ot_leave(void) {
    if(!R.recording || R.nots == 0) return;
    ot_t *ot = &R.ots[--R.nots];
    R.nmap = ot->map_mark; /* the inner stream's address may be reused by the next one */
    if(ot->sorting) return;
    if(ot->inner_buf >= 0 && ot->has_content) {
        /* Encode: the inner encode came first. Decode: the octets were read first. */
        wr_t *w = &R.writes[ot->content_write];
        buf_t *b = &R.bufs[ot->inner_buf];
        b->has_reloc = 1;
        b->reloc = w->pos;
        b->content_bits = w->n;
        b->content_frame = w->frame;
    } else if(ot->has_content) {
        /* Octets nobody decoded (an unknown extension, skipped): they stay one segment. */
        wr_t *w = &R.writes[ot->content_write];
        w->drop = 0;
        w->kind = "open-type-unknown";
    } else if(ot->inner_buf >= 0 && !R.decoding) {
        fail("an open type whose octets were never written");
    }
}

#define WRAP_OT_PUT(fn)                                                                                        \
    int __real_##fn(const asn_TYPE_descriptor_t *, const asn_per_constraints_t *, const void *, asn_per_outp_t *); \
    int __wrap_##fn(const asn_TYPE_descriptor_t *td, const asn_per_constraints_t *ct, const void *sptr,         \
                    asn_per_outp_t *po) {                                                                      \
        ot_enter(po, 0);                                                                                       \
        int r = __real_##fn(td, ct, sptr, po);                                                                 \
        ot_leave();                                                                                            \
        return r;                                                                                              \
    }
WRAP_OT_PUT(uper_open_type_put)
WRAP_OT_PUT(aper_open_type_put)

asn_dec_rval_t __real_uper_open_type_get(const asn_codec_ctx_t *, const asn_TYPE_descriptor_t *,
                                         const asn_per_constraints_t *, void **, asn_per_data_t *);
asn_dec_rval_t __wrap_uper_open_type_get(const asn_codec_ctx_t *ctx, const asn_TYPE_descriptor_t *td,
                                         const asn_per_constraints_t *ct, void **sptr, asn_per_data_t *pd) {
    ot_enter(pd, 0);
    asn_dec_rval_t r = __real_uper_open_type_get(ctx, td, ct, sptr, pd);
    ot_leave();
    return r;
}
int __real_uper_open_type_skip(const asn_codec_ctx_t *, asn_per_data_t *);
int __wrap_uper_open_type_skip(const asn_codec_ctx_t *ctx, asn_per_data_t *pd) {
    ot_enter(pd, 0);
    int r = __real_uper_open_type_skip(ctx, pd);
    ot_leave();
    return r;
}

/* ---- ld --wrap: SET OF sorting (X.691 22.1) ----
 * asn1c encodes every SET OF element on its own to sort them. UPER then
 * relays those octets (exact bit counts) in sorted order; APER encodes each
 * element again in place, so its sorting-pass encodes are never used. */
struct _el_buffer *__real_SET_OF__encode_sorted(const asn_TYPE_member_t *, const asn_anonymous_set_ *,
                                                enum SET_OF__encode_method);
struct _el_buffer *__wrap_SET_OF__encode_sorted(const asn_TYPE_member_t *elm, const asn_anonymous_set_ *list,
                                                enum SET_OF__encode_method method) {
    if(!R.recording || R.decoding || method == SOES_DER) return __real_SET_OF__encode_sorted(elm, list, method);
    const void *outer = R.depth ? R.frames[R.stack[R.depth - 1]].stream : NULL;
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

/* ------------------------------------------------------------------ assemble */

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
           || !strcmp(kind, "trailing-padding") || !strcmp(kind, "undecoded");
}

static int same_str(const char *a, const char *b) { return a == b || (a && b && !strcmp(a, b)); }

static void reset_recording(int codec, int decoding) {
    for(size_t i = 0; i < R.nframes; i++) {
        free(R.frames[i].path);
        free(R.frames[i].comp);
    }
    free(R.frames);
    free(R.writes);
    memset(&R, 0, sizeof R);
    R.codec = codec;
    R.decoding = decoding;
}

/* Decode frames carry a component each; their paths are built here. */
static void resolve_paths(void) {
    if(!R.decoding) return;
    for(size_t i = 0; i < R.nframes; i++) {
        fr_t *f = &R.frames[i];
        const char *c = f->comp ? f->comp : f->td->name;
        if(f->parent >= 0 && R.frames[f->parent].path) {
            size_t n = strlen(R.frames[f->parent].path) + strlen(c) + 1;
            f->path = malloc(n);
            snprintf(f->path, n, "%s%s", R.frames[f->parent].path, c);
        } else {
            f->path = strdup(c);
        }
    }
}

/* Turns the recording into frames and segments tiling [0, total_bits);
 * bits from `used_bits` on are trailing padding (or, after a failed decode,
 * undecoded). */
static void assemble(asn1viz_lane_t *lane, size_t used_bits, size_t total_bits, int failed) {
    resolve_paths();
    /* Buffers whose bits reach the output: the root, and every inner buffer
     * relocated into a live parent. APER's SET OF sorting-pass buffers are not. */
    int *alive = calloc((size_t)R.nbufs + 1, sizeof *alive);
    for(int b = 0; b < R.nbufs; b++) {
        if(b == 0) alive[b] = 1;
        else alive[b] = R.bufs[b].has_reloc && R.bufs[b].parent_buf >= 0 && alive[R.bufs[b].parent_buf];
        if(b > 0 && !R.bufs[b].has_reloc && !R.bufs[b].sorted && !failed && !R.error[0])
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
    lane->root_span_ok = nf > 0 && lane->frames[0].start == 0 && lane->frames[0].end == used_bits;

    /* Segments: every write, relocated; each inner buffer's own padding; the tail. */
    size_t cap = R.nwrites + (size_t)R.nbufs + 2, n = 0;
    asn1viz_segment_t *segs = calloc(cap, sizeof *segs);
    size_t reached = 0;
    for(size_t i = 0; i < R.nwrites; i++) {
        wr_t *w = &R.writes[i];
        if(w->drop || !alive[w->buf]) continue;
        segs[n].start = abs_pos(w->buf, w->pos);
        segs[n].len = w->n;
        segs[n].frame = w->frame >= 0 ? remap[w->frame] : 0;
        segs[n].kind = w->kind;
        segs[n].source = w->source;
        segs[n].member = w->member;
        if(segs[n].start + segs[n].len > reached) reached = segs[n].start + segs[n].len;
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
            if(segs[n].start + segs[n].len > reached) reached = segs[n].start + segs[n].len;
            n++;
        }
    }
    size_t tail = failed ? reached : used_bits;
    if(total_bits > tail) {
        segs[n].start = tail;
        segs[n].len = total_bits - tail;
        segs[n].frame = 0;
        segs[n].kind = failed ? "undecoded" : "trailing-padding";
        segs[n].source = ASN1VIZ_EXACT;
        n++;
    }
    free(alive);
    free(remap);
    qsort(segs, n, sizeof *segs, seg_cmp);

    /* Decoding: a CHOICE index's alternative is its first child's name. */
    for(size_t i = 0; i < n; i++) {
        if(segs[i].member || strcmp(segs[i].kind, "choice-index")) continue;
        for(size_t c = 0; c < lane->nframes; c++)
            if(lane->frames[c].parent == segs[i].frame) {
                const char *dot = strrchr(lane->frames[c].path, '.');
                segs[i].member = dot ? dot + 1 : NULL;
                break;
            }
    }

    /* Self-test: the segments tile [0, total_bits) exactly. */
    size_t at = 0;
    lane->tiles = 1;
    for(size_t i = 0; i < n; i++) {
        if(segs[i].start != at) {
            lane->tiles = 0;
            if(!R.error[0])
                snprintf(R.error, sizeof R.error, "segments do not tile: %s at bit %zu (expected %zu), %s %s in %s",
                         segs[i].start > at ? "gap" : "overlap", segs[i].start, at, segs[i].kind,
                         segs[i].source ? "(inferred)" : "", segs[i].frame >= 0 && (size_t)segs[i].frame < lane->nframes
                         ? lane->frames[segs[i].frame].path : "?");
            break;
        }
        at = segs[i].start + segs[i].len;
    }
    if(lane->tiles && at != total_bits) {
        lane->tiles = 0;
        if(!R.error[0]) snprintf(R.error, sizeof R.error, "segments end at bit %zu, not %zu", at, total_bits);
    }

    /* Merge runs of the same thing (a string's characters, a padding run). */
    size_t m = 0;
    for(size_t i = 0; i < n; i++) {
        if(m > 0) {
            asn1viz_segment_t *p = &segs[m - 1];
            if(p->frame == segs[i].frame && !strcmp(p->kind, segs[i].kind) && p->source == segs[i].source
               && same_str(p->member, segs[i].member) && mergeable(p->kind) && p->start + p->len == segs[i].start) {
                p->len += segs[i].len;
                continue;
            }
        }
        segs[m++] = segs[i];
    }
    lane->segments = segs;
    lane->nsegments = m;
}

/* ------------------------------------------------------------------ encode */

static asn_enc_rval_t encode(asn_TYPE_descriptor_t *pdu, const void *value, int codec, dyn_t *out) {
    memset(out, 0, sizeof *out);
    return codec == ASN1VIZ_APER ? aper_encode(pdu, NULL, value, dyn_cb, out)
                                 : uper_encode(pdu, NULL, value, dyn_cb, out);
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
    reset_recording(codec, 0);
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

    assemble(lane, lane->encoded_bits, lane->bits, 0);

    if(R.error[0]) snprintf(lane->error, sizeof lane->error, "%s", R.error);
    else if(!lane->matches_untraced) snprintf(lane->error, sizeof lane->error, "traced bytes differ from untraced bytes");
    else if(!lane->root_span_ok)
        snprintf(lane->error, sizeof lane->error, "the root frame does not span [0, %zu)", lane->encoded_bits);
    return lane->error[0] ? -1 : 0;
}

/* ------------------------------------------------------------------ decode */

/* The bit-counting variants (the _complete ones report octets). */
typedef asn_dec_rval_t (*decode_fn)(const asn_codec_ctx_t *, const asn_TYPE_descriptor_t *, void **, const void *,
                                    size_t, int, int);

static asn_dec_rval_t run_decode(asn_TYPE_descriptor_t *pdu, decode_fn fn, const uint8_t *data, size_t len) {
    asn_codec_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.max_stack_size = 256 * 1024;
    void *value = NULL;
    asn_dec_rval_t r = fn(&ctx, pdu, &value, data, len, 0, 0);
    ASN_STRUCT_FREE(*pdu, value);
    return r;
}

/* The X.691 rule a strict canonical check enforces in a type, as far as the
 * type tells (INTEGER_uper.c and constr_SEQUENCE_uper.c are where they live). */
static const char *rule_for(const char *type, int kind) {
    if(kind == K_SEQUENCE)
        return "X.691 19.5 / 19.9: a DEFAULT value encoded, or an extension bit set with no additions present";
    if(type && (strstr(type, "INTEGER") || strstr(type, "Serial") || strstr(type, "Version") || strstr(type, "ENUM")))
        return "X.691 11.3.6: an unconstrained INTEGER's contents are not in the fewest octets";
    return "a canonical-UPER check in this type";
}

int asn1viz_trace_decode_uper(asn_TYPE_descriptor_t *pdu, const uint8_t *data, size_t len, const char *source,
                              int canonical, asn1viz_lane_t *lane) {
    memset(lane, 0, sizeof *lane);
    lane->codec = ASN1VIZ_UPER;
    lane->imported = 1;
    lane->source = source;
    lane->bits = len * 8;
    lane->nbytes = len;
    lane->bytes = malloc(len ? len : 1);
    memcpy(lane->bytes, data, len);

    /* Untraced, lenient: what the decoder makes of it with the original tables. */
    int was = g_installed;
    asn1viz_trace_restore();
    asn_dec_rval_t r0 = run_decode(pdu, uper_decode_canonical_lenient, data, len);
    asn1viz_trace_install(was ? g_pdu : pdu);

    /* Strict, traced: where (if anywhere) the canonical checks fire. */
    char failed_path[512] = "";
    if(canonical) {
        reset_recording(ASN1VIZ_UPER, 1);
        R.recording = 1;
        asn_dec_rval_t rs = run_decode(pdu, uper_decode_canonical, data, len);
        R.recording = 0;
        resolve_paths();
        lane->canonical.present = 1;
        lane->canonical.frame = -1;
        lane->canonical.lenient = r0.code == RC_OK ? "accepted" : "rejected";
        lane->canonical.result = rs.code == RC_OK ? "ok" : "rejected";
        if(rs.code != RC_OK) {
            lane->canonical.bit = rs.consumed;
            lane->canonical.rule = "a canonical-UPER check";
            /* The deepest frame that failed: the check fired inside it. */
            int deepest = -1;
            for(size_t i = 0; i < R.nframes; i++)
                if(R.frames[i].code != RC_OK && (deepest < 0 || R.frames[i].depth > R.frames[deepest].depth))
                    deepest = (int)i;
            if(deepest >= 0) {
                snprintf(failed_path, sizeof failed_path, "%s", R.frames[deepest].path ? R.frames[deepest].path : "");
                lane->canonical.rule = rule_for(R.frames[deepest].td->name, kind_of(R.frames[deepest].td));
            }
        }
    }

    /* Lenient, traced: the layout. */
    reset_recording(ASN1VIZ_UPER, 1);
    R.recording = 1;
    asn_dec_rval_t r = run_decode(pdu, uper_decode_canonical_lenient, data, len);
    R.recording = 0;
    int failed = r.code != RC_OK;
    lane->encoded_bits = r.consumed;
    lane->matches_untraced = r.code == r0.code && r.consumed == r0.consumed;
    assemble(lane, r.consumed, lane->bits, failed);
    if(failed_path[0]) {
        for(size_t i = 0; i < lane->nframes; i++)
            if(!strcmp(lane->frames[i].path, failed_path)) {
                lane->canonical.frame = (int)i;
                break;
            }
    }
    if(failed) lane->root_span_ok = 1; /* nothing to compare a failed decode's span with */

    if(R.error[0]) snprintf(lane->error, sizeof lane->error, "%s", R.error);
    else if(!lane->matches_untraced) snprintf(lane->error, sizeof lane->error, "traced decode differs from untraced");
    else if(!lane->tiles) snprintf(lane->error, sizeof lane->error, "segments do not tile the input");
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
    if(l->imported) {
        fprintf(out, "%s  \"imported\": true,\n%s  \"source\": ", ind, ind);
        asn1viz_json_string(out, l->source);
        fputs(",\n", out);
    }
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
    fprintf(out, "%s  ],\n%s  \"canonical\": ", ind, ind);
    if(!l->canonical.present) {
        fputs("null", out);
    } else {
        fprintf(out, "{ \"result\": \"%s\", \"mode\": \"strict\", \"lenient\": \"%s\"", l->canonical.result,
                l->canonical.lenient);
        if(!strcmp(l->canonical.result, "rejected")) {
            fprintf(out, ", \"frame\": %d, \"bit\": %zu, \"rule\": ", l->canonical.frame, l->canonical.bit);
            asn1viz_json_string(out, l->canonical.rule);
        }
        fputs(" }", out);
    }
    fprintf(out, "\n%s}", ind);
}
