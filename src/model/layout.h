// asn1-layout/1 (RFC 0023 section 3): one value's encodings as lanes of
// frames (the span tree of encode calls) and segments (the leaves, which tile
// each lane's bits exactly). Read-only model; no engine.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "model/json.h"

namespace asn1viz {

struct LayoutFrame {
    int id = 0, parent = -1, depth = 0;
    std::string path, type;
    int64_t start = 0, end = 0;
};

struct LayoutSegment {
    int64_t start = 0, len = 0;
    int frame = 0;
    std::string kind;    // value, presence-bit, extension-bit, length, alignment, ...
    bool inferred = false;
    std::string member;  // for presence/extension bits and choice indexes
};

// The canonical-UPER overlay on an imported lane (RFC 0023 G4, section 2.7).
struct CanonicalCheck {
    std::string result;     // "ok", "rejected" or "warned"; empty when there is none
    std::string mode;       // "strict"
    std::string lenient;    // the lenient mode's answer: "accepted" or "rejected"
    int frame = -1;         // the frame open when the check fired
    int64_t bit = -1;       // the bit position the decoder had reached
    std::string rule;       // which X.691 rule, as far as the extractor can tell
    std::string message;
};

struct LayoutLane {
    std::string codec;      // uper, aper, or imported-uper
    std::string source;     // for an imported lane: the file it came from
    int64_t bits = 0, encodedBits = 0;
    std::vector<uint8_t> bytes;
    bool tiles = false, matchesUntraced = false, rootSpan = false;
    std::vector<LayoutFrame> frames;
    std::vector<LayoutSegment> segments;
    CanonicalCheck canonical;

    int bit(int64_t i) const;  // 0 or 1; -1 out of range
    std::string bitString(int64_t start, int64_t len, size_t maxChars = 64) const;
    std::string hexOfBits(int64_t start, int64_t len, size_t maxChars = 64) const;  // the octets the range touches
    int segmentAt(int64_t bit) const;                         // index or -1
    int frameByPath(const std::string& path) const;           // index or -1
};

struct Layout {
    std::string asn1cCommit, pdu, valueSource, valueSha256;
    std::vector<std::string> schemaFiles;
    std::vector<LayoutLane> lanes;
};

// Reads an asn1-layout/1 document. Throws std::runtime_error when it is not
// one, or is malformed.
Layout loadLayout(const json::Value& doc);
Layout loadLayoutFile(const std::string& path);

// The checks the extractor's self-test makes, repeated on the reader's side:
// segments tile [0, bits) in order, every frame and segment index is valid,
// and the bytes cover the bits. Returns "" when the lane is sound.
std::string validateLane(const LayoutLane& lane);

}  // namespace asn1viz
