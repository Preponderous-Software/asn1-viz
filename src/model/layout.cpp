#include "model/layout.h"

#include <algorithm>
#include <stdexcept>

namespace asn1viz {

namespace {

std::vector<uint8_t> fromHex(const std::string& hex) {
    if (hex.size() % 2) throw std::runtime_error("asn1-layout/1: odd-length hex");
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw std::runtime_error("asn1-layout/1: bad hex digit");
    };
    std::vector<uint8_t> out(hex.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<uint8_t>(nib(hex[2 * i]) << 4 | nib(hex[2 * i + 1]));
    return out;
}

bool flag(const json::Value& o, const char* key) {
    const json::Value& v = o.get(key);
    return v.type() == json::Value::Type::Bool && v.asBool();
}

}  // namespace

int LayoutLane::bit(int64_t i) const {
    if (i < 0 || i >= static_cast<int64_t>(bytes.size()) * 8) return -1;
    return (bytes[static_cast<size_t>(i / 8)] >> (7 - i % 8)) & 1;
}

std::string LayoutLane::bitString(int64_t start, int64_t len, size_t maxChars) const {
    std::string s;
    for (int64_t i = 0; i < len; ++i) {
        if (s.size() >= maxChars) {
            s += "...";
            break;
        }
        const int b = bit(start + i);
        s += b < 0 ? '?' : static_cast<char>('0' + b);
    }
    return s;
}

std::string LayoutLane::hexOfBits(int64_t start, int64_t len, size_t maxChars) const {
    static const char* d = "0123456789abcdef";
    std::string s;
    if (len <= 0) return s;
    const int64_t first = start / 8, last = (start + len - 1) / 8;
    for (int64_t i = first; i <= last && i < static_cast<int64_t>(bytes.size()); ++i) {
        if (!s.empty()) s += ' ';
        if (s.size() + 2 > maxChars) {
            s += "...";
            break;
        }
        s += d[bytes[static_cast<size_t>(i)] >> 4];
        s += d[bytes[static_cast<size_t>(i)] & 15];
    }
    return s;
}

int LayoutLane::segmentAt(int64_t b) const {
    auto it = std::upper_bound(segments.begin(), segments.end(), b,
                               [](int64_t v, const LayoutSegment& s) { return v < s.start; });
    if (it == segments.begin()) return -1;
    --it;
    return b < it->start + it->len ? static_cast<int>(it - segments.begin()) : -1;
}

int LayoutLane::frameByPath(const std::string& path) const {
    for (size_t i = 0; i < frames.size(); ++i)
        if (frames[i].path == path) return static_cast<int>(i);
    return -1;
}

Layout loadLayout(const json::Value& doc) {
    if (doc.str("format") != "asn1-layout/1") throw std::runtime_error("not an asn1-layout/1 document");
    Layout l;
    l.asn1cCommit = doc.get("asn1c").str("commit");
    const json::Value& schema = doc.get("schema");
    l.pdu = schema.str("pdu");
    if (schema.get("files").isArray())
        for (const json::Value& f : schema.at("files").items()) l.schemaFiles.push_back(f.asString());
    l.valueSource = doc.get("value").str("source");
    l.valueSha256 = doc.get("value").str("sha256");
    for (const json::Value& lv : doc.at("lanes").items()) {
        LayoutLane lane;
        lane.codec = lv.str("codec");
        lane.source = lv.str("source");
        lane.bits = lv.integer("bits");
        lane.encodedBits = lv.integer("encodedBits", lane.bits);
        lane.bytes = fromHex(lv.str("hex"));
        const json::Value& st = lv.get("selfTest");
        lane.tiles = flag(st, "tiles");
        lane.matchesUntraced = flag(st, "matchesUntraced");
        lane.rootSpan = flag(st, "rootSpan");
        for (const json::Value& fv : lv.at("frames").items()) {
            LayoutFrame f;
            f.id = static_cast<int>(fv.integer("id"));
            f.parent = static_cast<int>(fv.integer("parent", -1));
            f.depth = static_cast<int>(fv.integer("depth"));
            f.path = fv.str("path");
            f.type = fv.str("type");
            f.start = fv.integer("start");
            f.end = fv.integer("end");
            lane.frames.push_back(std::move(f));
        }
        for (const json::Value& sv : lv.at("segments").items()) {
            LayoutSegment s;
            s.start = sv.integer("start");
            s.len = sv.integer("len");
            s.frame = static_cast<int>(sv.integer("frame"));
            s.kind = sv.str("kind");
            s.inferred = sv.str("source") == "inferred";
            s.member = sv.str("member");
            lane.segments.push_back(std::move(s));
        }
        const json::Value& c = lv.get("canonical");
        if (c.isObject()) {
            lane.canonical.result = c.str("result");
            lane.canonical.mode = c.str("mode");
            lane.canonical.lenient = c.str("lenient");
            lane.canonical.frame = static_cast<int>(c.integer("frame", -1));
            lane.canonical.bit = c.integer("bit", -1);
            lane.canonical.rule = c.str("rule");
            lane.canonical.message = c.str("message");
        }
        l.lanes.push_back(std::move(lane));
    }
    return l;
}

Layout loadLayoutFile(const std::string& path) { return loadLayout(json::parseFile(path)); }

std::string validateLane(const LayoutLane& lane) {
    if (static_cast<int64_t>(lane.bytes.size()) * 8 < lane.bits) return "fewer bytes than bits";
    int64_t at = 0;
    for (size_t i = 0; i < lane.segments.size(); ++i) {
        const LayoutSegment& s = lane.segments[i];
        if (s.start != at) return "segment " + std::to_string(i) + " starts at " + std::to_string(s.start) +
                                  ", expected " + std::to_string(at);
        if (s.len <= 0) return "segment " + std::to_string(i) + " is empty";
        if (s.frame < 0 || s.frame >= static_cast<int>(lane.frames.size()))
            return "segment " + std::to_string(i) + " names no frame";
        at = s.start + s.len;
    }
    if (at != lane.bits) return "segments end at " + std::to_string(at) + ", not " + std::to_string(lane.bits);
    for (size_t i = 0; i < lane.frames.size(); ++i) {
        const LayoutFrame& f = lane.frames[i];
        if (f.parent >= static_cast<int>(i)) return "frame " + std::to_string(i) + " has a later parent";
        if (f.start > f.end || f.end > lane.bits) return "frame " + std::to_string(i) + " is out of range";
    }
    return "";
}

}  // namespace asn1viz
