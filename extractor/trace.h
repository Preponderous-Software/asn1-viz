/* The asn1-viz tracer (RFC 0023 section 2): per-field bit spans from an
 * unmodified asn1c codec.
 *
 *  - Tier 1: every type-level PER encode call goes through a shim written
 *    into the type's op table (in place, so BER's pointer comparisons on
 *    op tables stay true). Each call becomes a frame: path, type, span.
 *  - Tier 2: the bit writers and the length, normally-small and alignment
 *    helpers are wrapped at link time (-Wl,--wrap=<symbol>), so every write
 *    is seen with its exact position and width, and the helpers label theirs.
 *  - Tier 3: bits a type writes itself with bare per_put_few_bits (the
 *    SEQUENCE preamble, the CHOICE index, a leaf's extension bit) are
 *    labelled from the type descriptor in X.691 order and marked inferred.
 *
 * Open types (SEQUENCE extension additions, information-object open types)
 * are encoded by asn1c into a separate buffer first; the tracer relocates
 * that buffer's frames and writes to where its octets land in the parent.
 *
 * The tracer patches process-wide tables: it is for a single-threaded,
 * short-lived extractor process, and asn1viz_trace_restore() puts every
 * table back. */
#ifndef ASN1VIZ_TRACE_H
#define ASN1VIZ_TRACE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "asn_application.h"

enum asn1viz_codec { ASN1VIZ_UPER = 0, ASN1VIZ_APER = 1 };

/* Segment sources (RFC 0023 G2). */
enum asn1viz_source { ASN1VIZ_EXACT = 0, ASN1VIZ_INFERRED = 1 };

typedef struct {
    int id, parent;
    char *path;          /* Certificate.tbsCertificate.validity.notBefore */
    const char *type;    /* the descriptor's name */
    size_t start, end;   /* absolute bits, [start, end) */
    int depth;
} asn1viz_frame_t;

typedef struct {
    size_t start, len;
    int frame;
    const char *kind;    /* value, presence-bit, extension-bit, length, alignment, ... */
    int source;          /* enum asn1viz_source */
    const char *member;  /* presence/extension bits and choice indexes: the member they are about */
} asn1viz_segment_t;

typedef struct {
    int codec;
    size_t encoded_bits;   /* er.encoded */
    size_t bits;           /* octets * 8: the segments tile [0, bits) */
    uint8_t *bytes;
    size_t nbytes;
    asn1viz_frame_t *frames;
    size_t nframes;
    asn1viz_segment_t *segments;
    size_t nsegments;
    /* Self-test (RFC 0023 section 2.8). */
    int tiles;             /* segments tile [0, bits) with no gap or overlap */
    int matches_untraced;  /* traced bytes == untraced bytes */
    int root_span_ok;      /* the root frame spans [0, encoded_bits) */
    char error[256];       /* why the trace failed, if it did */
} asn1viz_lane_t;

/* Patches the op tables of every type reachable from `pdu`. Returns the
 * number of distinct op tables patched. */
int asn1viz_trace_install(asn_TYPE_descriptor_t *pdu);
/* Restores every patched table. */
void asn1viz_trace_restore(void);

/* Encodes `value` once untraced and once traced in `codec`, and fills
 * `lane`. Returns 0 when the encoding succeeded and every self-test check
 * passed; -1 otherwise (lane->error says why). */
int asn1viz_trace_encode(asn_TYPE_descriptor_t *pdu, const void *value, int codec, asn1viz_lane_t *lane);

void asn1viz_lane_free(asn1viz_lane_t *lane);

/* Writes `lane` as one asn1-layout/1 lane object (no trailing newline). */
void asn1viz_lane_write_json(FILE *out, const asn1viz_lane_t *lane, const char *indent);

/* JSON string with escaping. */
void asn1viz_json_string(FILE *out, const char *s);

#endif
