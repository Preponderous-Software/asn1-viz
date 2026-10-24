// The decoder side and the canonical-UPER overlay (RFC 0023 G4, section 2.7):
//  - the fork's own canonical sample, imported, lays out exactly as the
//    encoder side does, and the strict canonical decoder accepts it;
//  - the same certificate with a widened serialNumber is rejected by the
//    strict check in that field (X.691 11.3.6), accepted leniently, and
//    everything after the field moves by one octet.
#include <string>

#include "check.h"
#include "model/layout.h"

using namespace asn1viz;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: test_canonical IMPORTED-SAMPLE.json CANONICAL.json\n");
        return 2;
    }
    const Layout same = loadLayoutFile(argv[1]);
    CHECK(same.lanes.size() == 2);
    if (same.lanes.size() == 2) {
        const LayoutLane& enc = same.lanes[0];
        const LayoutLane& dec = same.lanes[1];
        CHECK(validateLane(dec).empty());
        CHECK(dec.source == "sample-Certificate-1.per");
        CHECK(dec.bytes == enc.bytes);
        CHECK(dec.encodedBits == enc.encodedBits);
        CHECK(dec.frames.size() == enc.frames.size());
        bool spans = dec.frames.size() == enc.frames.size();
        for (size_t i = 0; spans && i < enc.frames.size(); ++i)
            spans = dec.frames[i].path == enc.frames[i].path && dec.frames[i].start == enc.frames[i].start &&
                    dec.frames[i].end == enc.frames[i].end;
        CHECK(spans);
        CHECK(dec.canonical.result == "ok");
    }

    const Layout bad = loadLayoutFile(argv[2]);
    CHECK(bad.lanes.size() == 2);
    if (bad.lanes.size() == 2) {
        const LayoutLane& enc = bad.lanes[0];
        const LayoutLane& dec = bad.lanes[1];
        CHECK(validateLane(dec).empty());
        CHECK(dec.source == "sample-Certificate-1-noncanonical.per");
        CHECK(dec.canonical.result == "rejected");
        CHECK(dec.canonical.mode == "strict" && dec.canonical.lenient == "accepted");
        CHECK(dec.canonical.frame >= 0);
        if (dec.canonical.frame >= 0)
            CHECK(dec.frames[static_cast<size_t>(dec.canonical.frame)].path == "Certificate.tbsCertificate.serialNumber");
        CHECK(dec.canonical.rule.find("11.3.6") != std::string::npos);
        CHECK(dec.encodedBits == enc.encodedBits + 8);
        const int e = enc.frameByPath("Certificate.tbsCertificate.serialNumber");
        const int d = dec.frameByPath("Certificate.tbsCertificate.serialNumber");
        CHECK(e >= 0 && d >= 0);
        if (e >= 0 && d >= 0) {
            CHECK(dec.frames[d].start == enc.frames[e].start);
            CHECK(dec.frames[d].end == enc.frames[e].end + 8);
        }
        const int es = enc.frameByPath("Certificate.signature"), ds = dec.frameByPath("Certificate.signature");
        CHECK(es >= 0 && ds >= 0 && dec.frames[ds].start == enc.frames[es].start + 8);
    }
    TEST_MAIN_END();
}
