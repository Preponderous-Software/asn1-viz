#include "view/open.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>

#include "model/json.h"
#include "model/layout.h"
#include "model/schema.h"
#include "view/city_view.h"
#include "view/strip_view.h"

namespace asn1viz {

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
    throw std::runtime_error("not a document asn1-viz draws (format \"" + format + "\")");
}

std::string selectionOf(const Scene& scene) {
    if (const auto* s = dynamic_cast<const StripView*>(&scene)) return s->hasSelection() ? s->selectionPath() : "";
    if (const auto* c = dynamic_cast<const CityView*>(&scene))
        return c->selected() >= 0 ? c->city().schema().types[static_cast<size_t>(c->selected())].name : "";
    return "";
}

}  // namespace asn1viz
