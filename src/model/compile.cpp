#include "model/compile.h"

#include <stdexcept>

#include "model/schema.h"

namespace asn1viz {

namespace {

int typeOf(const json::Value& v) { return static_cast<int>(v.integer("type", -1)); }

std::map<std::string, int> histogram(const json::Value& v) {
    std::map<std::string, int> h;
    if (v.isObject())
        for (const auto& [k, n] : v.members()) h[k] = static_cast<int>(n.asInt());
    return h;
}

std::vector<std::string> strings(const json::Value& v) {
    std::vector<std::string> out;
    if (v.isArray())
        for (const json::Value& s : v.items()) out.push_back(s.asString());
    return out;
}

std::vector<CompileCopied> copied(const json::Value& v) {
    std::vector<CompileCopied> out;
    if (v.isArray())
        for (const json::Value& f : v.items()) {
            CompileCopied c;
            c.path = f.str("path");
            c.bytes = f.integer("bytes");
            c.lines = static_cast<int>(f.integer("lines"));
            out.push_back(std::move(c));
        }
    return out;
}

}  // namespace

std::map<std::string, int> CompileTrace::eventsByFn() const {
    std::map<std::string, int> h;
    for (const CompileEvent& e : events) ++h[e.fn];
    return h;
}

std::map<std::string, int> CompileTrace::linesByFn() const {
    std::map<std::string, int> h;
    for (const CompileFile& f : files)
        for (const auto& [fn, n] : f.byFn) h[fn] += n;
    return h;
}

int CompileTrace::totalLines() const {
    int n = 0;
    for (const CompileFile& f : files) n += f.lines;
    return n;
}

int CompileTrace::annotatedLines() const {
    int n = 0;
    for (const CompileFile& f : files) n += f.annotated();
    return n;
}

std::vector<int> CompileTrace::filesOf(int type) const {
    std::vector<int> out;
    for (size_t i = 0; i < files.size(); ++i)
        if (files[i].type == type) out.push_back(static_cast<int>(i));
    return out;
}

std::vector<int> CompileTrace::eventsOf(int type) const {
    std::vector<int> out;
    for (size_t i = 0; i < events.size(); ++i)
        if (events[i].type == type) out.push_back(static_cast<int>(i));
    return out;
}

bool CompileTrace::matches(const Schema& s) const {
    const int n = static_cast<int>(s.types.size());
    auto ok = [n](int t) { return t >= -1 && t < n; };
    for (const CompileParseStep& p : parse)
        if (!ok(p.type)) return false;
    for (const CompileEvent& e : events)
        if (!ok(e.type)) return false;
    for (const CompileFile& f : files)
        if (!ok(f.type)) return false;
    for (const auto& d : diffs)
        if (d.first < 0 || d.first >= n) return false;
    return true;
}

CompileTrace loadCompileTrace(const json::Value& doc) {
    if (doc.str("format") != "asn1-compile/1") throw std::runtime_error("not an asn1-compile/1 document");
    CompileTrace t;
    t.asn1c = doc.str("asn1c");
    t.schema = doc.str("schema");
    t.flags = strings(doc.get("flags"));
    t.inputs = strings(doc.get("inputs"));
    t.parseStatus = doc.str("parseStatus", "ok");
    if (doc.get("parse").isArray())
        for (const json::Value& p : doc.at("parse").items())
            t.parse.push_back({typeOf(p), static_cast<int>(p.integer("line"))});

    const json::Value& fix = doc.get("fix");
    t.fixStatus = fix.str("status");
    t.fixResult = static_cast<int>(fix.integer("result"));
    t.fixWarnings = static_cast<int>(fix.integer("warnings"));
    if (fix.get("events").isArray())
        for (const json::Value& e : fix.at("events").items()) {
            CompileEvent ev;
            ev.seq = static_cast<int>(e.integer("seq"));
            ev.fn = e.str("fn");
            ev.type = typeOf(e);
            ev.line = static_cast<int>(e.integer("line", -1));
            ev.src = e.str("src");
            ev.expr = e.str("expr");
            ev.file = e.str("file");
            ev.how = e.str("how");
            if (ev.seq != static_cast<int>(t.events.size()))
                throw std::runtime_error("asn1-compile/1: event seq out of order");
            t.events.push_back(std::move(ev));
        }
    if (fix.get("diffs").isObject())
        for (const auto& [k, v] : fix.at("diffs").members()) t.diffs[std::stoi(k)] = v.asString();

    const json::Value& comp = doc.get("compile");
    t.compileStatus = comp.str("status");
    t.functions = strings(comp.get("functions"));
    if (comp.get("files").isArray())
        for (const json::Value& f : comp.at("files").items()) {
            CompileFile cf;
            cf.path = f.str("path");
            cf.type = typeOf(f);
            cf.lines = static_cast<int>(f.integer("lines"));
            cf.unannotated = static_cast<int>(f.integer("unannotated"));
            cf.bytes = f.integer("bytes");
            cf.byFn = histogram(f.get("byFn"));
            cf.bySrc = histogram(f.get("bySrc"));
            t.files.push_back(std::move(cf));
        }

    const json::Value& save = doc.get("save");
    t.saveStatus = save.str("status");
    t.skeletons = copied(save.get("skeletons"));
    t.generated = copied(save.get("generated"));
    return t;
}

CompileTrace loadCompileTraceFile(const std::string& path) { return loadCompileTrace(json::parseFile(path)); }

}  // namespace asn1viz
