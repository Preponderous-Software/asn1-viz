// The asn1-compile/1 traces against the golden counts measured at the
// pinned asn1c (asn1c.ref): the fixer's events and its busiest reporters,
// what the fixer changed in the printed tree, and the generated files,
// lines and emitting functions. The fixer's debug text and the origin
// comments are not an API (RFC 0034 Risks): an asn1c.ref bump that changes
// them fails here, loudly, and these numbers are re-measured on purpose.
//
// Run on the committed datasets (every platform) and, on Linux, on the
// traces the build has just regenerated (compile_golden_generated).
#include <string>

#include "check.h"
#include "model/compile.h"
#include "model/schema.h"

using namespace asn1viz;

namespace {

struct Golden {
    int events, unattributed, nowProcessing, distinctFns;
    const char* topFn;
    int topFnEvents;
    const char* secondFn;
    int secondFnEvents;
    int changedAssignments;
    int files, attributedFiles, lines, annotated, functions;
    const char* topEmitter;
    int topEmitterLines;
    int skeletons;
};

void checkTrace(const std::string& dir, const Golden& g) {
    const CompileTrace t = loadCompileTraceFile(dir + "/compile.json");
    const Schema s = loadSchemaFile(dir + "/" + t.schema);
    std::printf("%s: %zu events, %zu files, %d lines (%d annotated), %zu functions, %zu skeletons\n", dir.c_str(),
                t.events.size(), t.files.size(), t.totalLines(), t.annotatedLines(), t.functions.size(),
                t.skeletons.size());
    CHECK(t.parseStatus == "ok" && t.fixStatus == "ok" && t.compileStatus == "ok" && t.saveStatus == "ok");
    CHECK(t.asn1c == s.asn1cCommit);
    CHECK(t.matches(s));
    CHECK(t.parse.size() == s.types.size());
    for (size_t i = 0; i < t.parse.size(); ++i)
        CHECK(t.parse[i].type == static_cast<int>(i) && t.parse[i].line == s.types[i].line);

    // Fix.
    CHECK(static_cast<int>(t.events.size()) == g.events);
    int unattributed = 0, now = 0;
    for (const CompileEvent& e : t.events) {
        unattributed += e.type < 0 ? 1 : 0;
        now += e.how == "processing" ? 1 : 0;
    }
    CHECK(unattributed == g.unattributed);
    CHECK(now == g.nowProcessing);
    const auto byFn = t.eventsByFn();
    CHECK(static_cast<int>(byFn.size()) == g.distinctFns);
    CHECK(byFn.count(g.topFn) && byFn.at(g.topFn) == g.topFnEvents);
    CHECK(byFn.count(g.secondFn) && byFn.at(g.secondFn) == g.secondFnEvents);
    for (const auto& [fn, n] : byFn) CHECK(n <= g.topFnEvents);
    CHECK(static_cast<int>(t.diffs.size()) == g.changedAssignments);
    for (const auto& [type, text] : t.diffs) CHECK(!text.empty() && text.rfind("@@ -", 0) == 0);

    // Compile.
    CHECK(static_cast<int>(t.files.size()) == g.files);
    int attributed = 0;
    for (const CompileFile& f : t.files) attributed += f.type >= 0 ? 1 : 0;
    CHECK(attributed == g.attributedFiles);
    CHECK(t.totalLines() == g.lines);
    CHECK(t.annotatedLines() == g.annotated);
    CHECK(static_cast<int>(t.functions.size()) == g.functions);
    const auto lines = t.linesByFn();
    CHECK(!t.functions.empty() && t.functions[0] == g.topEmitter);
    CHECK(lines.count(g.topEmitter) && lines.at(g.topEmitter) == g.topEmitterLines);
    // codeBytes in the schema is the bytes of each type's files.
    for (const SchemaType& ty : s.types) {
        int64_t bytes = 0;
        for (int f : t.filesOf(ty.id)) bytes += t.files[static_cast<size_t>(f)].bytes;
        CHECK(ty.codeBytes == bytes);
    }

    // Save.
    CHECK(static_cast<int>(t.skeletons.size()) == g.skeletons);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    // Measured at asn1c.ref b8971cc6. 251 unattributed events in both: the
    // asn1f_process preamble plus everything in asn1c's two standard modules
    // (16 of the 270 PKIX "Now processing" markers), which are not in the
    // schema. The two unattributed PKIX files are EXTERNAL.c/.h, from a
    // standard module.
    checkTrace(root + "/datasets/demo",
               {391, 251, 18, 15, "asn1constraint_resolve", 136, "asn1f_resolve_constraints", 88, 2,  //
                6, 4, 1093, 971, 23, "emit_member_table", 330, 311});
    // RFC 0034's Evidence (a newer local asn1c) measured 7,812 events, 20
    // reporters (asn1constraint_resolve 2,225, asn1f_resolve_constraints
    // 1,070), 128 of 811 printed lines changed in 53 assignments, 254 files,
    // 24,489 lines (19,405 annotated) from 26 functions (emit_member_table
    // 4,783) and 268 skeletons. The pin's code generator writes more.
    checkTrace(root + "/datasets/pkix",
               {7835, 251, 270, 20, "asn1constraint_resolve", 2225, "asn1f_resolve_constraints", 1070, 53,  //
                254, 252, 26292, 21208, 27, "emit_member_table", 4783, 311});
    {
        // What the fixer did to the PKIX tree, in the printed lines.
        const CompileTrace t = loadCompileTraceFile(root + "/datasets/pkix/compile.json");
        const Schema s = loadSchemaFile(root + "/datasets/pkix/schema.json");
        const int x520 = s.typeByName("X520name");
        CHECK(x520 >= 0 && t.diffs.count(x520));
        if (t.diffs.count(x520)) {
            const std::string& d = t.diffs.at(x520);
            CHECK(d.find("-    utf8String\t UTF8String (SIZE(1..ub-name))") != std::string::npos);
            CHECK(d.find("+    utf8String\t UTF8String (SIZE(1..32768))") != std::string::npos);
        }
        // Certificate's code: its own .c and .h, by asn1c's file naming.
        const int cert = s.typeByName("Certificate");
        const auto files = t.filesOf(cert);
        CHECK(files.size() == 2);
        for (int f : files) CHECK(t.files[static_cast<size_t>(f)].path.rfind("Certificate.", 0) == 0);
        CHECK(s.types[static_cast<size_t>(cert)].codeBytes > 0);
        // Every phase_1_1 marker names the assignment it attributes.
        for (const CompileEvent& e : t.events)
            if (e.how == "processing" && e.type >= 0) CHECK(s.types[static_cast<size_t>(e.type)].name == e.expr);
    }
    TEST_MAIN_END();
}
