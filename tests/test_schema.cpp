// The committed asn1-schema/1 files read and say what RFC 0024's Evidence
// measured: PKIX (2 modules, 126 types, 101 type-to-type references, 6 of
// them across modules, 1 IMPORTS pair, no recursion) and the fork's
// 92-circular-loops-OK test schema (13 types, 30 references, 3 cycles, the
// largest of 6 types).
#include <algorithm>
#include <string>

#include "check.h"
#include "model/schema.h"

using namespace asn1viz;

int main(int argc, char** argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    const Schema p = loadSchemaFile(root + "/datasets/pkix/schema.json");
    CHECK(p.modules.size() == 2);
    CHECK(p.modules[1].imports.size() == 1 && p.modules[1].imports[0] == "PKIX1Explicit88");
    int types = 0, typeRefs = 0, cross = 0;
    for (const SchemaType& t : p.types) types += t.flat() ? 0 : 1;
    for (const SchemaRef& r : p.refs) {
        const SchemaType& a = p.types[static_cast<size_t>(r.from)];
        const SchemaType& b = p.types[static_cast<size_t>(r.to)];
        if (a.flat() || b.flat()) continue;
        ++typeRefs;
        if (a.module != b.module) ++cross;
    }
    CHECK(types == 126);
    CHECK(typeRefs == 101);
    CHECK(cross == 6);
    CHECK(p.sccs.empty());
    const int cert = p.typeByName("Certificate");
    CHECK(cert >= 0);
    if (cert >= 0) {
        CHECK(p.types[static_cast<size_t>(cert)].kind == "SEQUENCE");
        CHECK(p.types[static_cast<size_t>(cert)].members == 3);
        // What Certificate pulls in: TBSCertificate and AlgorithmIdentifier at one hop.
        const auto one = p.reach(cert, 1);
        CHECK(one.count(p.typeByName("TBSCertificate")) && one.count(p.typeByName("AlgorithmIdentifier")));
        CHECK(p.reach(cert, 3).size() > one.size());
    }
    const auto where = p.locations();
    CHECK(where.count("TBSCertificate") && where.at("TBSCertificate").rfind("rfc3280-PKIX1Explicit88.asn1:", 0) == 0);

    const Schema r = loadSchemaFile(root + "/datasets/recursion/schema.json");
    CHECK(r.types.size() == 13);
    CHECK(r.refs.size() == 30);
    CHECK(r.sccs.size() == 3);
    size_t largest = 0;
    for (const auto& c : r.sccs) largest = std::max(largest, c.size());
    CHECK(largest == 6);
    for (size_t i = 0; i < r.sccs.size(); ++i)
        for (int t : r.sccs[i]) CHECK(r.types[static_cast<size_t>(t)].scc == static_cast<int>(i));
    TEST_MAIN_END();
}
