/* asn1-viz-extract: one value's UPER and APER bit layouts as asn1-layout/1.
 *
 *   asn1-viz-extract --value FILE --value-codec der|ber|xer|jer|uper|aper
 *                    [--codecs uper,aper] [--schema NAME]... [--out FILE]
 *
 * The PDU is fixed at build time (one extractor per schema and PDU; see
 * asn1viz_add_extractor in CMakeLists.txt). The value is decoded with the
 * generated codec, then encoded once per codec with the tracer on. Every
 * lane must pass the self-test (segments tile the stream, traced bytes equal
 * untraced bytes, the root span equals the encoded length); if one fails,
 * nothing is written and the exit status is 1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asn_application.h"
#include "sha256.h"
#include "trace.h"

extern asn_TYPE_descriptor_t *const asn1viz_pdu;

#ifndef ASN1VIZ_ASN1C_COMMIT
#define ASN1VIZ_ASN1C_COMMIT "unknown"
#endif

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if(!f) return NULL;
    size_t cap = 4096, n = 0;
    uint8_t *buf = malloc(cap);
    for(;;) {
        if(n == cap) buf = realloc(buf, cap *= 2);
        size_t r = fread(buf + n, 1, cap - n, f);
        if(!r) break;
        n += r;
    }
    fclose(f);
    *len = n;
    return buf;
}

static int syntax_of(const char *codec, enum asn_transfer_syntax *out) {
    static const struct {
        const char *name;
        enum asn_transfer_syntax ts;
    } t[] = {{"der", ATS_DER},          {"ber", ATS_BER},           {"xer", ATS_BASIC_XER},
             {"jer", ATS_JER},          {"uper", ATS_UNALIGNED_BASIC_PER}, {"aper", ATS_ALIGNED_BASIC_PER}};
    for(size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if(!strcmp(codec, t[i].name)) {
            *out = t[i].ts;
            return 0;
        }
    return -1;
}

static void usage(void) {
    fprintf(stderr,
            "usage: asn1-viz-extract --value FILE --value-codec der|ber|xer|jer|uper|aper\n"
            "                        [--codecs uper,aper] [--schema NAME]... [--out FILE]\n");
}

int main(int argc, char **argv) {
    const char *value_path = NULL, *value_codec = NULL, *codecs = "uper,aper", *out_path = NULL;
    const char *schemas[32];
    int nschemas = 0;
    for(int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if(!strcmp(a, "--value") && v) value_path = argv[++i];
        else if(!strcmp(a, "--value-codec") && v) value_codec = argv[++i];
        else if(!strcmp(a, "--codecs") && v) codecs = argv[++i];
        else if(!strcmp(a, "--out") && v) out_path = argv[++i];
        else if(!strcmp(a, "--schema") && v && nschemas < 32) schemas[nschemas++] = argv[++i];
        else {
            usage();
            return 2;
        }
    }
    if(!value_path || !value_codec) {
        usage();
        return 2;
    }
    enum asn_transfer_syntax ts;
    if(syntax_of(value_codec, &ts)) {
        fprintf(stderr, "asn1-viz-extract: unknown value codec %s\n", value_codec);
        return 2;
    }
    size_t len = 0;
    uint8_t *data = read_file(value_path, &len);
    if(!data) {
        fprintf(stderr, "asn1-viz-extract: cannot read %s\n", value_path);
        return 1;
    }

    /* Untrusted input: bound the decoder's stack (RFC 0023, Security). */
    asn_codec_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.max_stack_size = 256 * 1024;
    void *value = NULL;
    asn_dec_rval_t dr = asn_decode(&ctx, ts, asn1viz_pdu, &value, data, len);
    if(dr.code != RC_OK) {
        fprintf(stderr, "asn1-viz-extract: %s did not decode as %s %s (consumed %zu of %zu bytes)\n", value_path,
                value_codec, asn1viz_pdu->name, dr.consumed, len);
        return 1;
    }

    int ncodecs = 0, codec_list[2];
    if(strstr(codecs, "uper")) codec_list[ncodecs++] = ASN1VIZ_UPER;
    if(strstr(codecs, "aper")) codec_list[ncodecs++] = ASN1VIZ_APER;
    if(!ncodecs) {
        fprintf(stderr, "asn1-viz-extract: --codecs names neither uper nor aper\n");
        return 2;
    }

    int tables = asn1viz_trace_install(asn1viz_pdu);
    asn1viz_lane_t lanes[2];
    int failed = 0;
    for(int i = 0; i < ncodecs; i++) {
        if(asn1viz_trace_encode(asn1viz_pdu, value, codec_list[i], &lanes[i])) {
            fprintf(stderr, "asn1-viz-extract: %s self-test failed: %s\n", codec_list[i] ? "APER" : "UPER",
                    lanes[i].error);
            failed = 1;
        } else {
            fprintf(stderr, "asn1-viz-extract: %s %zu bits, %zu frames, %zu segments, %d op tables patched\n",
                    codec_list[i] ? "APER" : "UPER", lanes[i].encoded_bits, lanes[i].nframes, lanes[i].nsegments,
                    tables);
        }
    }
    asn1viz_trace_restore();
    if(failed) return 1;

    FILE *out = out_path ? fopen(out_path, "wb") : stdout;
    if(!out) {
        fprintf(stderr, "asn1-viz-extract: cannot write %s\n", out_path);
        return 1;
    }
    uint8_t digest[32];
    asn1viz_sha256(data, len, digest);
    fprintf(out, "{\n  \"format\": \"asn1-layout/1\",\n");
    fprintf(out, "  \"asn1c\": { \"repo\": \"dmccoystephenson/asn1c\", \"commit\": \"%s\" },\n", ASN1VIZ_ASN1C_COMMIT);
    fprintf(out, "  \"schema\": { \"files\": [");
    for(int i = 0; i < nschemas; i++) {
        if(i) fputs(", ", out);
        asn1viz_json_string(out, base_name(schemas[i]));
    }
    fprintf(out, "], \"pdu\": ");
    asn1viz_json_string(out, asn1viz_pdu->name);
    fprintf(out, " },\n  \"value\": { \"source\": ");
    asn1viz_json_string(out, base_name(value_path));
    fprintf(out, ", \"codec\": \"%s\", \"sha256\": \"", value_codec);
    for(int i = 0; i < 32; i++) fprintf(out, "%02x", digest[i]);
    fprintf(out, "\" },\n  \"lanes\": [\n    ");
    for(int i = 0; i < ncodecs; i++) {
        if(i) fputs(",\n    ", out);
        asn1viz_lane_write_json(out, &lanes[i], "    ");
        asn1viz_lane_free(&lanes[i]);
    }
    fprintf(out, "\n  ]\n}\n");
    if(out != stdout) fclose(out);
    ASN_STRUCT_FREE(*asn1viz_pdu, value);
    free(data);
    return 0;
}
