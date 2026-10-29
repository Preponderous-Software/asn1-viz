// asn1-compile/1 (RFC 0034 section 2): one asn1c run, stage by stage
// (parse, fix, compile, save), as asn1-viz-compile-trace records it. Type ids
// are those of the asn1-schema/1 file the trace names (`schema`); -1 stands
// for null (unattributed). Read-only model; no engine.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "model/json.h"

namespace asn1viz {

struct Schema;

struct CompileParseStep {
    int type = -1, line = 0;
};

// One line of the fixer's debug trace.
struct CompileEvent {
    int seq = 0;
    std::string fn;    // the reporting function (asn1f_resolve_constraints, ...)
    int type = -1;     // the assignment it is attributed to, or -1
    int line = -1;     // the schema line the message names, or -1
    std::string src;   // asn1c source "asn1fix.c:489", or empty
    std::string expr;  // the identifier the message names, or empty (additive)
    std::string file;  // the schema file the message names, or empty (additive)
    std::string how;   // the attribution rule: processing, line, name, current, none (additive)
};

// One generated file, with its lines per emitting compiler function.
struct CompileFile {
    std::string path;
    int type = -1;
    int lines = 0, unannotated = 0;
    int64_t bytes = 0;                    // additive
    std::map<std::string, int> byFn;      // emit_member_table -> lines
    std::map<std::string, int> bySrc;     // "asn1c_C.c:4166" -> lines (additive)
    int annotated() const { return lines - unannotated; }
};

struct CompileCopied {
    std::string path;
    int64_t bytes = 0;
    int lines = 0;  // generated helper files only
};

struct CompileTrace {
    std::string asn1c, schema;
    std::vector<std::string> flags, inputs;
    std::string parseStatus = "ok";
    std::vector<CompileParseStep> parse;

    std::string fixStatus;
    int fixResult = 0, fixWarnings = 0;
    std::vector<CompileEvent> events;
    std::map<int, std::string> diffs;  // only the assignments the fixer changed

    std::string compileStatus;
    std::vector<CompileFile> files;
    std::vector<std::string> functions;  // busiest first

    std::string saveStatus;
    std::vector<CompileCopied> skeletons, generated;

    // Derived.
    std::map<std::string, int> eventsByFn() const;
    std::map<std::string, int> linesByFn() const;
    int totalLines() const;
    int annotatedLines() const;
    std::vector<int> filesOf(int type) const;   // indexes into files
    std::vector<int> eventsOf(int type) const;  // indexes into events
    // Every type id in the trace names a type of `schema`.
    bool matches(const Schema& schema) const;
};

CompileTrace loadCompileTrace(const json::Value& doc);
CompileTrace loadCompileTraceFile(const std::string& path);

}  // namespace asn1viz
