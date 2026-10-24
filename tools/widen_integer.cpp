// asn1-viz-widen-integer: makes one unconstrained INTEGER in a UPER encoding
// non-minimal (one redundant leading octet, 0x00 or 0xff by its sign), the
// thing X.691 11.3.6 forbids in canonical PER. It reads the field's bits from
// an asn1-layout/1 file, so it needs no codec. Used to build the canonical
// overlay's demo input from a canonical sample.
//
//   asn1-viz-widen-integer LAYOUT.json LANE FIELD-PATH OUT.per
//
// The field must be an unconstrained INTEGER with a single-octet length
// (fewer than 127 contents octets) that is not inside an open type.
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "model/layout.h"

using namespace asn1viz;

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: asn1-viz-widen-integer LAYOUT.json LANE FIELD-PATH OUT.per\n");
        return 2;
    }
    try {
        const Layout layout = loadLayoutFile(argv[1]);
        const size_t laneIndex = static_cast<size_t>(std::stoul(argv[2]));
        if (laneIndex >= layout.lanes.size()) throw std::runtime_error("no such lane");
        const LayoutLane& lane = layout.lanes[laneIndex];
        if (lane.codec != "uper") throw std::runtime_error("the lane is not UPER");
        const int f = lane.frameByPath(argv[3]);
        if (f < 0) throw std::runtime_error(std::string("no field ") + argv[3]);
        const LayoutFrame& fr = lane.frames[static_cast<size_t>(f)];
        // The field's own length and value segments.
        const LayoutSegment *len = nullptr, *val = nullptr;
        for (const LayoutSegment& s : lane.segments) {
            if (s.frame != f) continue;
            if (s.kind == "length" && !len) len = &s;
            else if (s.kind == "value" && !val) val = &s;
        }
        if (!len || !val || len->len != 8 || val->start != len->start + 8 || val->len % 8)
            throw std::runtime_error("the field is not an unconstrained INTEGER with an 8-bit length");
        for (int p = lane.frames[static_cast<size_t>(f)].parent; p >= 0; p = lane.frames[static_cast<size_t>(p)].parent)
            for (const LayoutSegment& s : lane.segments)
                if (s.frame == p && s.kind == "open-type-length")
                    throw std::runtime_error("the field is inside an open type");
        int octets = 0;
        for (int i = 0; i < 8; ++i) octets = octets << 1 | lane.bit(len->start + i);
        if (octets + 1 >= 128) throw std::runtime_error("the contents are too long for this tool");
        std::vector<int> bits;
        for (int64_t i = 0; i < len->start; ++i) bits.push_back(lane.bit(i));
        for (int i = 7; i >= 0; --i) bits.push_back((octets + 1) >> i & 1);
        const int sign = lane.bit(val->start);
        for (int i = 0; i < 8; ++i) bits.push_back(sign);  // 0x00 before a positive, 0xff before a negative
        for (int64_t i = val->start; i < lane.encodedBits; ++i) bits.push_back(lane.bit(i));
        while (bits.size() % 8) bits.push_back(0);
        std::vector<unsigned char> out(bits.size() / 8);
        for (size_t i = 0; i < bits.size(); ++i) out[i / 8] = static_cast<unsigned char>(out[i / 8] | bits[i] << (7 - i % 8));
        std::ofstream o(argv[4], std::ios::binary);
        o.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
        if (!o) throw std::runtime_error(std::string("cannot write ") + argv[4]);
        std::printf("%s: %s widened from %d to %d octets at bit %lld (%zu octets)\n", argv[4], fr.path.c_str(), octets,
                    octets + 1, static_cast<long long>(val->start), out.size());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "asn1-viz-widen-integer: %s\n", e.what());
        return 1;
    }
    return 0;
}
