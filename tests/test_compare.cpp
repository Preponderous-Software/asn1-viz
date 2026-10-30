// Compare mode (RFC 0034 G4) on the committed comparison traces, against the
// golden counts measured at the pinned asn1c (asn1c.ref): the same schema
// compiled with one flag changed. demo: -no-gen-OER (what OER costs per
// type). PKIX: -fwide-types (the flag the PKIX layouts are built with). Then
// the fixer marks on an edited trace, a pair that is not one schema, and the
// compare city's floors.
//
// Run on the committed datasets (every platform) and, on Linux, on the
// traces the build has just regenerated (compare_golden_generated).
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

#include "check.h"
#include "model/compare.h"
#include "model/schema.h"
#include "scene/compare_city.h"

using namespace asn1viz;

namespace {

int fnLines(const CompileCompare& c, int type, const std::string& fn, bool b) {
    const int i = c.functionIndex(fn);
    if (i < 0) return -1;
    const CompareEntry& e = type >= 0 ? c.type(type) : c.shared();
    return (b ? e.b : e.a)[static_cast<size_t>(i)];
}

bool hasColor(const MeshBuilder& m, Rgb c) {
    const std::vector<float>& col = m.colors();
    const size_t stride = m.vertexCount() > 0 ? col.size() / static_cast<size_t>(m.vertexCount()) : 3;
    for (size_t i = 0; i + 2 < col.size(); i += stride)
        if (std::fabs(col[i] - c.r) < 1e-4f && std::fabs(col[i + 1] - c.g) < 1e-4f && std::fabs(col[i + 2] - c.b) < 1e-4f)
            return true;
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string root = argc > 1 ? argv[1] : ".";

    {
        // demo: without OER, every type loses its OER constraint tables and
        // asn1c copies 20 fewer skeletons (the *_oer.c/h support code).
        const std::string dir = root + "/datasets/demo";
        const Schema s = loadSchemaFile(dir + "/schema.json");
        const CompileCompare c(s, loadCompileTraceFile(dir + "/compile.json"),
                               loadCompileTraceFile(dir + "/compile-no-gen-OER.json"));
        CHECK(CompileCompare::flagsLabel(c.a()) == "default flags");
        CHECK(CompileCompare::flagsLabel(c.b()) == "-no-gen-OER");
        CHECK(c.a().asn1c == c.b().asn1c);
        CHECK(c.typeCount() == 2 && c.typesChanged() == 2);
        CHECK(c.totalA() == 1093 && c.totalB() == 1047);
        const int msg = s.typeByName("Msg"), item = s.typeByName("Item");
        CHECK(c.type(msg).linesA == 519 && c.type(msg).linesB == 494 && c.type(msg).delta() == -25);
        CHECK(c.type(item).linesA == 237 && c.type(item).linesB == 221);
        CHECK(fnLines(c, msg, "emit_member_OER_constraints", false) == 25);
        CHECK(fnLines(c, msg, "emit_member_OER_constraints", true) == 0);
        CHECK(fnLines(c, -1, "emit_member_OER_constraints", false) == 5);  // EXTERNAL, from a standard module
        CHECK(c.shared().linesA == 337 && c.shared().linesB == 332);
        // Only the OER function changed.
        const std::vector<int> la = c.linesA(), lb = c.linesB();
        for (size_t i = 0; i < la.size(); ++i)
            CHECK((la[i] != lb[i]) == (c.functionName(static_cast<int>(i)) == "emit_member_OER_constraints"));
        CHECK(c.typesFixerDiffers() == 0);  // a code generator flag: the fixer runs the same
        CHECK(c.a().skeletons.size() == 311 && c.b().skeletons.size() == 291);

        // The fixer marks: an edited copy of B whose fixer changed Item
        // differently, and named Msg once more.
        CompileTrace b = loadCompileTraceFile(dir + "/compile-no-gen-OER.json");
        b.diffs[item] += "+    -- edited\n";
        CompileEvent extra = b.events.back();
        extra.seq = static_cast<int>(b.events.size());
        extra.type = msg;
        b.events.push_back(extra);
        const CompileCompare edited(s, loadCompileTraceFile(dir + "/compile.json"), b);
        CHECK(edited.type(item).diffDiffers && edited.type(item).fixerDiffers());
        CHECK(!edited.type(msg).diffDiffers && edited.type(msg).eventsB == edited.type(msg).eventsA + 1);
        CHECK(edited.typesFixerDiffers() == 2);
        CompareCity city(s, edited, CityStyle{});
        MeshBuilder marks;
        city.buildMarks(CompilePick{}, marks);
        CHECK(hasColor(marks, fixerDiffColor()));

        // Not the same schema: the PKIX trace against the demo's.
        bool threw = false;
        try {
            CompileCompare(s, loadCompileTraceFile(dir + "/compile.json"),
                           loadCompileTraceFile(root + "/datasets/pkix/compile.json"));
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
    }

    {
        // PKIX: -fwide-types (INTEGER_t instead of native long) changes 9
        // types. Constrained INTEGERs lose their native-type code and gain
        // constraint checking; TBSCertificate's DEFAULT v1 takes longer to
        // inline; BasicConstraints loses most.
        const std::string dir = root + "/datasets/pkix";
        const Schema s = loadSchemaFile(dir + "/schema.json");
        const CompileCompare c(s, loadCompileTraceFile(dir + "/compile.json"),
                               loadCompileTraceFile(dir + "/compile-fwide-types.json"));
        std::printf("pkix: %d types changed, %d -> %d lines\n", c.typesChanged(), c.totalA(), c.totalB());
        CHECK(CompileCompare::flagsLabel(c.b()) == "-fwide-types");
        CHECK(c.typesChanged() == 9);
        CHECK(c.totalA() == 26292 && c.totalB() == 26172);
        CHECK(c.typesFixerDiffers() == 0);
        const int basic = s.typeByName("BasicConstraints"), tbs = s.typeByName("TBSCertificate"),
                  skip = s.typeByName("SkipCerts"), inhibit = s.typeByName("InhibitAnyPolicy"),
                  reason = s.typeByName("CRLReason"), cert = s.typeByName("Certificate");
        CHECK(c.type(basic).linesA == 282 && c.type(basic).linesB == 197);
        CHECK(fnLines(c, basic, "emit_type_DEF", false) == 65 && fnLines(c, basic, "emit_type_DEF", true) == 27);
        CHECK(c.type(tbs).delta() == 2);
        CHECK(fnLines(c, tbs, "try_inline_default", false) == 24 && fnLines(c, tbs, "try_inline_default", true) == 26);
        CHECK(c.type(skip).delta() == -6);
        CHECK(fnLines(c, skip, "asn1c_emit_constraint_checking_code", false) == 15);
        CHECK(fnLines(c, skip, "asn1c_emit_constraint_checking_code", true) == 20);
        CHECK(fnLines(c, skip, "asn1c_lang_C_type_common_INTEGER", true) == 0);
        // Changed with no net change: lines moved between functions.
        CHECK(c.type(inhibit).changed() && c.type(inhibit).delta() == 0);
        CHECK(c.type(reason).linesA == 170 && c.type(reason).linesB == 149);
        CHECK(!c.type(cert).changed());

        // The city: floors tagged by type and function, growth and shrinkage
        // both drawn, each building as tall as max(A, B) per function.
        const CompareCity city(s, c, CityStyle{});
        bool growth = false, shrink = false, bandOfBasic = false;
        for (size_t d = 0; d < city.city().districts().size(); ++d) {
            MeshBuilder m;
            city.buildDistrict(d, m);
            growth = growth || hasColor(m, growthColor());
            shrink = shrink || hasColor(m, shrinkColor());
            for (const TriTag& t : m.tags()) {
                const CompilePick p = decodeTag(t.tag);
                if (p.kind == CompilePick::Band && p.type == basic &&
                    c.functionName(p.fn) == "asn1c_lang_C_type_common_INTEGER")
                    bandOfBasic = true;
            }
        }
        CHECK(growth && shrink && bandOfBasic);
        float want = 0;
        const CompareEntry& e = c.type(basic);
        for (size_t i = 0; i < e.a.size(); ++i) {
            const int common = std::min(e.a[i], e.b[i]), diff = std::abs(e.b[i] - e.a[i]);
            if (common > 0) want += std::max(0.02f, static_cast<float>(common) * city.lineScale());
            if (diff > 0) want += std::max(0.02f, static_cast<float>(diff) * city.lineScale());
        }
        CHECK(std::fabs(city.height(basic) - want) < 1e-4f);
        CHECK(city.height(cert) > 0);
    }
    TEST_MAIN_END();
}
