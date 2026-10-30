// asn1-viz in a browser: the same views on Strata's WebGL 2 backend
// (Emscripten), with the redistributable datasets built in (RFC 0023 P5,
// RFC 0024 C3). Nothing is compiled in the page: the layouts and schemas are
// the committed asn1-layout/1, asn1-schema/1 and asn1-compile/1 files under
// /datasets.
//
// The page (web/shell.html) picks a dataset and offers buttons for what keys
// do on the desktop; mouse and touch go to the canvas through SDL as on the
// desktop (drag to orbit, right-drag or two fingers to pan, wheel to zoom,
// click or tap to pick).
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <string>

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

#include "view/compare_view.h"
#include "view/compile_view.h"
#include "view/open.h"
#include "view/shell.h"

using namespace asn1viz;

namespace {

struct Dataset {
    const char* id;
    const char* file;
    const char* pick;
    int lane, links, hops;
    const char* compareWith = nullptr;  // compile: a second trace of the same schema (RFC 0034 G4)
};

// The order the page's buttons use.
const Dataset kSets[] = {
    {"strips", "/datasets/pkix/certificate.layout.json", "Certificate.tbsCertificate.validity", 0, 2, 1},
    {"canonical", "/datasets/pkix/certificate-canonical.layout.json", "Certificate.tbsCertificate.serialNumber", 1, 3, 1},
    {"demo", "/datasets/demo/msg.layout.json", "Msg.period", 0, 0, 1},
    {"city", "/datasets/pkix/schema.json", "Certificate", 0, 0, 3},
    {"recursion", "/datasets/recursion/schema.json", "Everything", 0, 0, 2},
    // Compared with the same pin's run without OER: what OER costs per type.
    {"compile", "/datasets/demo/compile.json", "Msg", 0, 0, 1, "/datasets/demo/compile-no-gen-OER.json"},
    // Not built in: the page fetches pkix-compile.json and writes it here
    // first. Its comparison (-fwide-types) is for the desktop.
    {"compile-pkix", "/datasets/pkix/compile.json", "Certificate", 0, 0, 1},
};
constexpr int kSetCount = static_cast<int>(sizeof kSets / sizeof kSets[0]);

struct App {
    std::unique_ptr<strata::Engine> engine;
    std::unique_ptr<Scene> scene;
    std::unique_ptr<Shell> shell;
    int dataset = -1;
    bool compact = false;
    double cssW = 0, cssH = 0;  // the canvas in CSS pixels (the page's coordinates)
    double winW = 0, winH = 0;  // SDL's window size (its pointer coordinates)
    std::string status;
};

App* app = nullptr;

int toggleCompare();

// SDL's pointer events are in its window's units: the size the page asked
// for, which is less than the CSS size when the page caps a high device pixel
// ratio (a DPR-3 phone); the drawable is that times the device pixel ratio.
// Text on the canvas is scaled by drawable pixels per CSS pixel, and on a
// narrow page the pick panel is the page's (a sheet under the canvas), not
// the canvas's.
void fitPointer() {
    if (!app->shell || app->cssW < 1 || app->cssH < 1 || app->winW < 1 || app->winH < 1) return;
    int32_t dw = 0, dh = 0;
    app->engine->drawableSize(dw, dh);
    app->shell->setPointerScale(static_cast<float>(dw / app->winW), static_cast<float>(dh / app->winH));
    const float ui = static_cast<float>(dw / app->cssW);
    app->shell->setUiScale(app->compact ? ui * 0.75f : ui);  // a phone: room for a few more words
    if (app->scene) app->scene->setPanelOnCanvas(!app->compact);
}

void frame(void*) {
    if (!app->shell) return;
    try {
        StrataEvent e;
        while (app->engine->pollEvent(e)) {
            // C on a lone trace with a comparison to open: open it (a pair takes C itself).
            if (e.type == STRATA_EVENT_KEY_DOWN && e.scancode == 6 && dynamic_cast<CompileView*>(app->scene.get()) &&
                app->dataset >= 0 && kSets[app->dataset].compareWith) {
                toggleCompare();
                continue;
            }
            app->shell->handle(e);
        }
        app->shell->frame();
        app->engine->present();
    } catch (const std::exception& ex) {
        // Said once, not every frame.
        static std::string last;
        if (last != ex.what()) std::fprintf(stderr, "asn1-viz: %s\n", ex.what());
        last = ex.what();
    }
}

void jsonString(std::string& out, const std::string& s) {
    out += '"';
    for (char ch : s) {
        if (ch == '"' || ch == '\\') {
            out += '\\';
            out += ch;
        } else if (static_cast<unsigned char>(ch) < 0x20) {
            out += ' ';
        } else {
            out += ch;
        }
    }
    out += '"';
}

// Replaces the scene, keeping the engine (its meshes cleared).
bool replaceScene(const char* what, const std::function<std::unique_ptr<Scene>()>& open) {
    try {
        app->shell.reset();
        app->scene.reset();
        app->engine->clearMeshes();
        app->scene = open();
        app->shell = std::make_unique<Shell>(*app->engine, *app->scene);
        fitPointer();
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "asn1-viz: cannot open %s: %s\n", what, e.what());
        return false;
    }
}

// Compare mode on the compilation dataset: opens the pair the first time
// (the second trace is only read then), and switches it after. 1: comparing.
int toggleCompare() {
    if (!app || !app->scene) return 0;
    if (auto* p = dynamic_cast<CompilePair*>(app->scene.get())) {
        p->setComparing(!p->comparing());
        return p->comparing() ? 1 : 0;
    }
    if (app->dataset < 0 || !kSets[app->dataset].compareWith) return 0;
    const Dataset& d = kSets[app->dataset];
    // The second trace is not built in: the page fetches it, then asks again.
    if (!std::ifstream(d.compareWith)) {
        emscripten_run_script("if (Module.onCompareWanted) Module.onCompareWanted()");
        return 0;
    }
    OpenOptions o;
    o.pick = d.pick;
    o.compact = app->compact;
    return replaceScene(d.compareWith, [&]() { return openCompare(d.file, d.compareWith, o, true); }) ? 1 : 0;
}

}  // namespace

extern "C" {

// Opens dataset `index` (see kSets); 1 on success.
EMSCRIPTEN_KEEPALIVE int asn1viz_open(int index) {
    if (!app || index < 0 || index >= kSetCount) return 0;
    const Dataset& d = kSets[index];
    OpenOptions o;
    o.pick = d.pick;
    o.lane = d.lane;
    o.links = d.links;
    o.hops = d.hops;
    o.compact = app->compact;
    if (!replaceScene(d.file, [&]() { return openScene(d.file, o); })) return 0;
    app->dataset = index;
    return 1;
}
// Compare mode (RFC 0034 G4) on or off, on a dataset that has a comparison; 1: on.
EMSCRIPTEN_KEEPALIVE int asn1viz_compare() { return toggleCompare(); }
// The canvas changed size: it is cssW x cssH CSS pixels, drawn at w x h
// (the page caps the device pixel ratio); `compact` (a narrow page) leaves
// out the legend and moves the pick panel to the page.
EMSCRIPTEN_KEEPALIVE void asn1viz_resize(double w, double h, double cssW, double cssH, int compact) {
    if (!app || w < 1 || h < 1 || cssW < 1 || cssH < 1) return;
    app->compact = compact != 0;
    app->cssW = cssW;
    app->cssH = cssH;
    app->winW = w;
    app->winH = h;
    try {
        if (app->shell) app->shell->resize(static_cast<int32_t>(w), static_cast<int32_t>(h));
        else app->engine->resize(static_cast<int32_t>(w), static_cast<int32_t>(h));
        fitPointer();
        if (app->shell) app->shell->home();  // frame the view for the new shape (a phone turned)
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "asn1-viz: resize: %s\n", ex.what());
    }
}
// A key press from a page button (an SDL scancode, as the keyboard sends).
EMSCRIPTEN_KEEPALIVE void asn1viz_key(int scancode) {
    if (!app || !app->shell) return;
    StrataEvent e{};
    e.type = STRATA_EVENT_KEY_DOWN;
    e.scancode = scancode;
    app->shell->handle(e);
}
// The page's ?bench mode flies the camera along a fixed path: Home, then
// turned yawDeg about the target, at pitchDeg above the horizon, with the
// distance scaled. Absolute, so the path is the same whatever the frame rate.
EMSCRIPTEN_KEEPALIVE void asn1viz_bench_view(double yawDeg, double pitchDeg, double distanceScale) {
    if (!app || !app->shell || distanceScale <= 0) return;
    app->shell->home();
    OrbitCamera& c = app->shell->camera();
    c.yawDeg += static_cast<float>(yawDeg);
    c.pitchDeg = static_cast<float>(pitchDeg);
    c.distance *= static_cast<float>(distanceScale);
}
// The compilation view's scrubber: seeks to fraction f along the bar (the
// page's range input, as the canvas's bar is drawn); no-op in other views.
EMSCRIPTEN_KEEPALIVE void asn1viz_seek(double f) {
    if (!app || !app->scene) return;
    if (auto* v = replayOf(*app->scene)) v->seekBar(static_cast<float>(f));
}
EMSCRIPTEN_KEEPALIVE void asn1viz_zoom(double factor) {
    if (app && app->shell && factor > 0) app->shell->camera().distance *= static_cast<float>(factor);
}
// Picks what is under a point of the canvas, in CSS pixels.
EMSCRIPTEN_KEEPALIVE void asn1viz_click(double x, double y) {
    if (!app || !app->shell) return;
    int32_t dw = 0, dh = 0;
    app->engine->drawableSize(dw, dh);
    const double sx = app->cssW > 0 ? dw / app->cssW : 1, sy = app->cssH > 0 ? dh / app->cssH : 1;
    app->shell->pickAt(static_cast<float>(x * sx), static_cast<float>(y * sy));
}
EMSCRIPTEN_KEEPALIVE const char* asn1viz_status() {
    if (!app) return "{}";
    std::string o = "{\"dataset\":";
    jsonString(o, app->dataset >= 0 ? kSets[app->dataset].id : "");
    o += ",\"selected\":";
    jsonString(o, app->scene ? selectionOf(*app->scene) : "");
    if (app->dataset >= 0 && kSets[app->dataset].compareWith) {
        const auto* p = app->scene ? dynamic_cast<const CompilePair*>(app->scene.get()) : nullptr;
        o += ",\"compare\":{\"available\":true,\"on\":";
        o += p && p->comparing() ? "true}" : "false}";
    }
    if (const auto* v = app->scene ? replayOf(*app->scene) : nullptr) {
        o += ",\"replay\":";
        o += v->statusJson();
    }
    o += ",\"panel\":[";
    if (app->scene) {
        bool first = true;
        for (const PanelLine& l : app->scene->panel(60)) {
            if (!first) o += ',';
            first = false;
            char rgb[16];
            std::snprintf(rgb, sizeof rgb, "#%02x%02x%02x", static_cast<int>(l.color[0] * 255),
                          static_cast<int>(l.color[1] * 255), static_cast<int>(l.color[2] * 255));
            o += "{\"text\":";
            jsonString(o, l.text);
            o += ",\"color\":\"";
            o += rgb;
            o += "\",\"small\":";
            o += l.size < 12 ? "true" : "false";
            o += '}';
        }
    }
    o += "]}";
    app->status = std::move(o);
    return app->status.c_str();
}

}  // extern "C"

int main() {
    static App a;
    app = &a;
    double cw = 0, ch = 0;
    emscripten_get_element_css_size("#canvas", &cw, &ch);
    if (cw < 1 || ch < 1) {
        cw = 1280;
        ch = 720;
    }
    strata::EngineDesc desc;
    desc.surface = STRATA_SURFACE_WINDOW;
    desc.width = static_cast<int32_t>(cw);
    desc.height = static_cast<int32_t>(ch);
    desc.title = "asn1-viz";
    try {
        app->engine = strata::Engine::create(desc);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "asn1-viz: cannot start the renderer: %s\n", ex.what());
        std::string js = "if (Module.onRendererFailed) Module.onRendererFailed(";
        jsonString(js, ex.what());
        js += ")";
        emscripten_run_script(js.c_str());
        return 1;
    }
    app->compact = cw < 1000;
    app->cssW = cw;
    app->cssH = ch;
    app->winW = cw;  // the window was created at the CSS size
    app->winH = ch;
    asn1viz_open(0);
    emscripten_run_script("if (Module.onViewerReady) Module.onViewerReady()");
    emscripten_set_main_loop_arg(frame, nullptr, 0, false);
    return 0;
}
