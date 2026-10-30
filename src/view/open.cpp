#include "view/open.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>

#include "model/compile.h"
#include "model/json.h"
#include "model/layout.h"
#include "model/schema.h"
#include "view/city_view.h"
#include "view/compare_view.h"
#include "view/compile_view.h"
#include "view/strip_view.h"

namespace asn1viz {

namespace {

// The asn1-schema/1 file a trace replays over: --schema, else the one it
// names, beside it.
std::string schemaFileOf(const std::string& file, const json::Value& doc, const OpenOptions& a) {
    const std::string named = doc.str("schema");
    if (named.empty()) throw std::runtime_error("asn1-compile/1: no schema named");
    const size_t slash = file.find_last_of("/\\");
    const bool absolute = named[0] == '/' || (named.size() > 1 && named[1] == ':');
    return !a.schema.empty() ? a.schema
           : absolute        ? named
                             : (slash == std::string::npos ? std::string() : file.substr(0, slash + 1)) + named;
}

}  // namespace

std::unique_ptr<Scene> openScene(const std::string& file, const OpenOptions& a) {
    const json::Value doc = json::parseFile(file);
    const std::string format = doc.str("format");
    if (format == "asn1-layout/1") {
        const Layout layout = loadLayout(doc);
        for (const LayoutLane& lane : layout.lanes) {
            const std::string bad = validateLane(lane);
            if (!bad.empty()) throw std::runtime_error(lane.codec + " lane: " + bad);
        }
        StripStyle style;
        int64_t most = 0;
        for (const LayoutLane& lane : layout.lanes) most = std::max(most, lane.bits);
        style.rowBits = a.rowBits > 0 ? a.rowBits : most <= 256 ? 32 : 128;
        if (most <= 256) style.laneGap = 6;
        auto view = std::make_unique<StripView>(layout, style);
        view->setLinkDepth(a.links);
        view->setCompact(a.compact);
        if (!a.pick.empty() && !view->selectPath(a.pick, static_cast<size_t>(a.lane)))
            throw std::runtime_error("no field " + a.pick + " in lane " + std::to_string(a.lane));
        std::string schema = a.schema;
        if (schema.empty()) {
            const size_t slash = file.find_last_of("/\\");
            const std::string beside = (slash == std::string::npos ? std::string() : file.substr(0, slash + 1)) + "schema.json";
            if (std::ifstream(beside)) schema = beside;
        }
        if (!schema.empty()) view->setTypeLocations(loadSchemaFile(schema).locations());
        return view;
    }
    if (format == "asn1-schema/1") {
        auto view = std::make_unique<CityView>(loadSchema(doc), CityStyle{});
        view->setHops(a.hops);
        view->setShowRecursion(a.loops);
        view->setCompact(a.compact);
        if (!a.pick.empty() && !view->selectName(a.pick)) throw std::runtime_error("no type " + a.pick);
        return view;
    }
    if (format == "asn1-compile/1") {
        // The city the trace replays over: the asn1-schema/1 file it names, beside it.
        const std::string schemaFile = schemaFileOf(file, doc, a);
        const Schema schema = loadSchemaFile(schemaFile);
        const CompileTrace trace = loadCompileTrace(doc);
        if (!trace.matches(schema)) throw std::runtime_error("asn1-compile/1: a type id is not one of " + schemaFile + "'s");
        auto view = std::make_unique<CompileView>(schema, trace, CityStyle{});
        view->setCompact(a.compact);
        if (a.hideLookups) view->replay().setLookupFiltered(true);
        if (a.at >= 0) view->replay().seekFraction(a.at);
        else view->replay().setPlaying(true);
        if (!a.pick.empty() && !view->selectName(a.pick)) throw std::runtime_error("no type " + a.pick);
        return view;
    }
    throw std::runtime_error("not a document asn1-viz draws (format \"" + format + "\")");
}

std::unique_ptr<Scene> openCompare(const std::string& fileA, const std::string& fileB, const OpenOptions& a,
                                   bool comparing) {
    const json::Value docA = json::parseFile(fileA), docB = json::parseFile(fileB);
    if (docA.str("format") != "asn1-compile/1" || docB.str("format") != "asn1-compile/1")
        throw std::runtime_error("--compare takes two asn1-compile/1 traces");
    // The city is A's schema; B must be a trace of the same one.
    const std::string schemaFile = schemaFileOf(fileA, docA, a);
    const Schema schema = loadSchemaFile(schemaFile);
    CompileTrace ta = loadCompileTrace(docA), tb = loadCompileTrace(docB);
    if (!ta.matches(schema)) throw std::runtime_error(fileA + ": a type id is not one of " + schemaFile + "'s");
    if (!tb.matches(schema)) throw std::runtime_error(fileB + ": not a trace of " + schemaFile);
    auto replay = std::make_unique<CompileView>(schema, ta, CityStyle{});
    replay->setCompact(a.compact);
    if (a.hideLookups) replay->replay().setLookupFiltered(true);
    if (a.at >= 0) replay->replay().seekFraction(a.at);
    else replay->replay().toEnd();  // the finished run, the state the comparison is of
    auto compare = std::make_unique<CompareView>(schema, CompileCompare(schema, std::move(ta), std::move(tb)), CityStyle{});
    compare->setCompact(a.compact);
    auto pair = std::make_unique<CompilePair>(std::move(replay), std::move(compare), comparing);
    if (!a.pick.empty() && !pair->selectName(a.pick)) throw std::runtime_error("no type or floor " + a.pick);
    return pair;
}

std::string selectionOf(const Scene& scene) {
    if (const auto* p = dynamic_cast<const CompilePair*>(&scene)) return p->selectionName();
    if (const auto* s = dynamic_cast<const StripView*>(&scene)) return s->hasSelection() ? s->selectionPath() : "";
    if (const auto* v = dynamic_cast<const CompileView*>(&scene)) return v->selectionName();
    if (const auto* c = dynamic_cast<const CityView*>(&scene))
        return c->selected() >= 0 ? c->city().schema().types[static_cast<size_t>(c->selected())].name : "";
    return "";
}

}  // namespace asn1viz
