// The committed PKIX layout (datasets/pkix/certificate.layout.json) reads,
// tiles, and says what the RFC 3280 sample certificate is. Runs everywhere
// (the viewer's input), including Windows and wasm.
#include <string>

#include "check.h"
#include "model/layout.h"

using namespace asn1viz;

int main(int argc, char** argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    const Layout l = loadLayoutFile(root + "/datasets/pkix/certificate.layout.json");
    CHECK(l.pdu == "Certificate");
    CHECK(l.valueSource == "sample-Certificate-1.der");
    CHECK(l.asn1cCommit.size() == 40);
    CHECK(l.lanes.size() == 2);
    if (l.lanes.size() != 2) {
        TEST_MAIN_END();
    }
    const LayoutLane& u = l.lanes[0];
    const LayoutLane& a = l.lanes[1];
    CHECK(u.codec == "uper" && a.codec == "aper");
    for (const LayoutLane* lane : {&u, &a}) {
        CHECK(validateLane(*lane).empty());
        CHECK(lane->tiles && lane->matchesUntraced && lane->rootSpan);
        CHECK(lane->frames.front().path == "Certificate");
        CHECK(lane->frameByPath("Certificate.tbsCertificate.validity.notBefore") >= 0);
        CHECK(lane->frameByPath("Certificate.signature") >= 0);
        // Bit 0 is TBSCertificate's first presence bit: version (DEFAULT v1, present: v3).
        const int s0 = lane->segmentAt(0);
        CHECK(s0 == 0);
        CHECK(lane->segments[0].kind == "presence-bit" && lane->segments[0].inferred);
        CHECK(lane->segments[0].member == "version");
        CHECK(lane->bit(0) == 1);
    }
    // The sizes the fork's samples have: UPER 1,031 octets, APER 1,043.
    CHECK(u.bytes.size() == 1031 && a.bytes.size() == 1043);
    CHECK(u.encodedBits == 8244 && a.encodedBits == 8344);
    CHECK(u.segments.back().kind == "trailing-padding");
    // APER aligns where UPER does not: the same field starts later.
    const int ub = u.frameByPath("Certificate.tbsCertificate.serialNumber");
    const int ab = a.frameByPath("Certificate.tbsCertificate.serialNumber");
    CHECK(ub >= 0 && ab >= 0);
    if (ub >= 0 && ab >= 0) CHECK(a.frames[ab].start > u.frames[ub].start);
    // Bits and hex of a range.
    CHECK(u.bitString(0, 4) == "1001");
    CHECK(u.hexOfBits(0, 8).size() == 2);
    CHECK(u.segmentAt(u.bits) == -1);
    TEST_MAIN_END();
}
