// The extractor against what is known (Linux, where it builds):
//  - the demo message laid out exactly as RFC 0023's spike measured it;
//  - the PKIX certificate's lanes byte for byte equal to the fork's own
//    sample encodings (examples/sample.source.PKIX1/*.per, *.aper).
#include <fstream>
#include <iterator>
#include <string>

#include "check.h"
#include "model/layout.h"

using namespace asn1viz;

namespace {

std::string hex(const std::vector<uint8_t>& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t c : b) {
        s += d[c >> 4];
        s += d[c & 15];
    }
    return s;
}

std::vector<uint8_t> readFile(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool span(const LayoutLane& l, const std::string& path, int64_t start, int64_t end) {
    const int f = l.frameByPath(path);
    return f >= 0 && l.frames[f].start == start && l.frames[f].end == end;
}

const LayoutSegment* kindAt(const LayoutLane& l, int64_t bit) {
    const int s = l.segmentAt(bit);
    return s >= 0 ? &l.segments[s] : nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: test_extractor DEMO.json PKIX.json PKIX.per PKIX.aper\n");
        return 2;
    }
    const Layout demo = loadLayoutFile(argv[1]);
    CHECK(demo.lanes.size() == 2);
    const LayoutLane& u = demo.lanes[0];
    const LayoutLane& a = demo.lanes[1];
    CHECK(validateLane(u).empty() && validateLane(a).empty());
    CHECK(hex(u.bytes) == "caa0401903a34915402040203000");
    CHECK(hex(a.bytes) == "c02a800200c810686922a80102010180");
    CHECK(u.encodedBits == 107 && a.encodedBits == 128);
    // Member spans (RFC 0023, Evidence).
    CHECK(span(u, "Msg.id", 2, 10));
    CHECK(span(u, "Msg.period", 10, 35));
    CHECK(span(u, "Msg.name", 35, 53));
    CHECK(span(u, "Msg.items", 53, 83));
    CHECK(span(u, "Msg.items[0].a", 59, 62));
    CHECK(span(u, "Msg.items[1].b", 64, 83));
    // The extension addition: length at 91/112, content at 99/120.
    CHECK(kindAt(u, 91) && kindAt(u, 91)->kind == "open-type-length");
    CHECK(span(u, "Msg.extra", 99, 100));
    CHECK(kindAt(a, 112) && kindAt(a, 112)->kind == "open-type-length");
    CHECK(span(a, "Msg.extra", 120, 121));
    CHECK(kindAt(u, 100) && kindAt(u, 100)->kind == "open-type-padding");
    // Tier 3 labels, inferred: Msg's extension bit and presence bit, the
    // extension bitmap, period's extension bit, the CHOICE indexes.
    CHECK(kindAt(u, 0)->kind == "extension-bit" && kindAt(u, 0)->inferred);
    CHECK(kindAt(u, 1)->kind == "presence-bit" && kindAt(u, 1)->member == "name");
    CHECK(kindAt(u, 90)->kind == "extension-bitmap" && kindAt(u, 90)->member == "extra");
    CHECK(kindAt(u, 10)->kind == "extension-bit");
    CHECK(kindAt(u, 58)->kind == "choice-index" && kindAt(u, 58)->member == "a");
    // Tier 2 labels, exact: APER's padding before id, lengths.
    CHECK(kindAt(a, 2)->kind == "alignment" && !kindAt(a, 2)->inferred);
    CHECK(kindAt(u, 11)->kind == "length" && !kindAt(u, 11)->inferred);
    CHECK(kindAt(u, 83)->kind == "extension-bitmap-length");

    const Layout pkix = loadLayoutFile(argv[2]);
    CHECK(pkix.lanes.size() == 2);
    if (pkix.lanes.size() == 2) {
        CHECK(pkix.lanes[0].bytes == readFile(argv[3]));
        CHECK(pkix.lanes[1].bytes == readFile(argv[4]));
        CHECK(validateLane(pkix.lanes[0]).empty() && validateLane(pkix.lanes[1]).empty());
    }
    TEST_MAIN_END();
}
