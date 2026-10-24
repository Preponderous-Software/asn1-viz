// asn1-viz: the desktop viewer.
//
//   asn1-viz [FILE] [options]
//
// FILE is an asn1-layout/1 document (bit strips, RFC 0023); with no FILE,
// datasets/pkix/certificate.layout.json is opened if it is there.
//
// Options:
//   --pick PATH          select a field by path (e.g. Certificate.tbsCertificate.validity)
//   --lane N             ...in lane N (default 0)
//   --click X Y          pick what is under pixel (X, Y), as a mouse click would
//   --links D            link every field of tree depth D across lanes (0: off)
//   --row-bits N         bits per row (default: 256, or 32 for a short encoding)
//   --yaw D --pitch D --zoom F --focus     camera adjustments from the home view
//   --target X Z                           ...looking at (X, 0, Z)
//   --width W --height H
//   --headless --shot PATH                 render one frame to a PNG and exit
//   --headless --orbit DIR --frames N      N frames turning once around, DIR/frame-000.png ...
//   --headless --tour DIR [--frames N]     visit each top-level field in turn (N frames per field)
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>

#include "model/json.h"
#include "model/layout.h"
#include "view/shell.h"
#include "view/strip_view.h"

using namespace asn1viz;

namespace {

struct Args {
    std::string file;
    bool headless = false;
    std::string shot, orbitDir, tourDir;
    int frames = 36;
    int width = 1280, height = 720;
    std::string pick;
    int lane = 0;
    float clickX = -1, clickY = -1;
    int links = 0;
    int rowBits = 0;
    float yaw = NAN, pitch = NAN, zoom = 1;
    bool focus = false;
    float targetX = NAN, targetZ = NAN;
};

[[noreturn]] void usage(int code) {
    std::fprintf(stderr,
                 "usage: asn1-viz [FILE.layout.json] [--pick PATH] [--lane N] [--click X Y] [--links D]\n"
                 "                [--row-bits N] [--yaw D] [--pitch D] [--zoom F] [--focus]\n"
                 "                [--width W --height H] [--headless (--shot PATH | --orbit DIR [--frames N])]\n");
    std::exit(code);
}

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) usage(2);
            return argv[++i];
        };
        if (s == "--headless") a.headless = true;
        else if (s == "--shot") a.shot = next();
        else if (s == "--orbit") a.orbitDir = next();
        else if (s == "--tour") a.tourDir = next();
        else if (s == "--frames") a.frames = std::atoi(next());
        else if (s == "--width") a.width = std::atoi(next());
        else if (s == "--height") a.height = std::atoi(next());
        else if (s == "--pick") a.pick = next();
        else if (s == "--lane") a.lane = std::atoi(next());
        else if (s == "--click") {
            a.clickX = static_cast<float>(std::atof(next()));
            a.clickY = static_cast<float>(std::atof(next()));
        } else if (s == "--links") a.links = std::atoi(next());
        else if (s == "--row-bits") a.rowBits = std::atoi(next());
        else if (s == "--yaw") a.yaw = static_cast<float>(std::atof(next()));
        else if (s == "--pitch") a.pitch = static_cast<float>(std::atof(next()));
        else if (s == "--zoom") a.zoom = static_cast<float>(std::atof(next()));
        else if (s == "--focus") a.focus = true;
        else if (s == "--target") {
            a.targetX = static_cast<float>(std::atof(next()));
            a.targetZ = static_cast<float>(std::atof(next()));
        }
        else if (s == "--version") {
            std::printf("asn1-viz %s\n", ASN1VIZ_VERSION);
            std::exit(0);
        } else if (s == "-h" || s == "--help") usage(0);
        else if (!s.empty() && s[0] != '-' && a.file.empty()) a.file = s;
        else {
            std::fprintf(stderr, "asn1-viz: unknown argument %s\n", s.c_str());
            usage(2);
        }
    }
    if (a.headless && a.shot.empty() && a.orbitDir.empty() && a.tourDir.empty()) {
        std::fprintf(stderr, "asn1-viz: --headless needs --shot PATH, --orbit DIR or --tour DIR\n");
        usage(2);
    }
    if (a.file.empty()) {
        a.file = "datasets/pkix/certificate.layout.json";
        if (!std::ifstream(a.file)) {
            std::fprintf(stderr, "asn1-viz: no FILE given and %s is not here\n", a.file.c_str());
            usage(2);
        }
    }
    return a;
}

std::unique_ptr<Scene> makeScene(const Args& a, const json::Value& doc) {
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
        if (!a.pick.empty() && !view->selectPath(a.pick, static_cast<size_t>(a.lane)))
            throw std::runtime_error("no field " + a.pick + " in lane " + std::to_string(a.lane));
        return view;
    }
    throw std::runtime_error("not a document asn1-viz draws (format \"" + format + "\")");
}

// Visits the fields of the first lane at depths 1 and 2, in stream order,
// easing the camera from one to the next (for the README's animation).
void tour(Shell& shell, strata::Engine& engine, StripView& view, const Args& a) {
    view.setCompact(a.width < 1000);
    const LayoutLane& lane = view.scene().layout().lanes.front();
    std::vector<std::string> stops;
    for (const LayoutFrame& f : lane.frames)
        if (f.depth == 2 || (f.depth == 1 && f.parent == 0 && lane.frames.size() < 20)) stops.push_back(f.path);
    const OrbitCamera start = shell.camera();
    OrbitCamera from = start;
    int n = 0;
    const int per = std::max(2, a.frames);
    for (size_t i = 0; i < stops.size(); ++i) {
        view.selectPath(stops[i], 0);
        Vec3 target;
        view.focus(target);
        OrbitCamera to = start;
        to.target = target;
        to.distance = start.distance * 0.42f;
        to.pitchDeg = 48;
        to.yawDeg = start.yawDeg - 18 + 36 * static_cast<float>(i) / static_cast<float>(std::max<size_t>(1, stops.size() - 1));
        for (int k = 0; k < per; ++k) {
            float t = std::min(1.0f, static_cast<float>(k) / (static_cast<float>(per) * 0.6f));
            t = t * t * (3 - 2 * t);
            OrbitCamera& c = shell.camera();
            c = from;
            c.target = {from.target.x + (to.target.x - from.target.x) * t, 0, from.target.z + (to.target.z - from.target.z) * t};
            c.distance = from.distance + (to.distance - from.distance) * t;
            c.pitchDeg = from.pitchDeg + (to.pitchDeg - from.pitchDeg) * t;
            c.yawDeg = from.yawDeg + (to.yawDeg - from.yawDeg) * t;
            shell.frame();
            char name[64];
            std::snprintf(name, sizeof name, "/frame-%03d.png", n++);
            engine.saveScreenshotPng(a.tourDir + name);
        }
        from = shell.camera();
    }
    std::printf("wrote %d frames (%zu fields) to %s\n", n, stops.size(), a.tourDir.c_str());
}

void adjustCamera(Shell& shell, const Args& a) {
    OrbitCamera& c = shell.camera();
    if (!std::isnan(a.yaw)) c.yawDeg = a.yaw;
    if (!std::isnan(a.pitch)) c.pitchDeg = a.pitch;
    c.distance *= a.zoom;
    if (a.focus) shell.lookAtFocus();
    if (!std::isnan(a.targetX)) c.target = {a.targetX, 0, a.targetZ};
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse(argc, argv);
    try {
        const json::Value doc = json::parseFile(args.file);
        std::unique_ptr<Scene> scene = makeScene(args, doc);
        strata::EngineDesc desc;
        desc.surface = args.headless ? STRATA_SURFACE_HEADLESS : STRATA_SURFACE_WINDOW;
        desc.width = args.width;
        desc.height = args.height;
        desc.title = "asn1-viz";
        auto engine = strata::Engine::create(desc);
        Shell shell(*engine, *scene);
        adjustCamera(shell, args);
        if (args.clickX >= 0) shell.pickAt(args.clickX, args.clickY);
        if (args.headless) {
            if (!args.shot.empty()) {
                shell.frame();
                engine->saveScreenshotPng(args.shot);
                std::printf("wrote %s\n", args.shot.c_str());
            }
            if (!args.orbitDir.empty()) {
                const float yaw0 = shell.camera().yawDeg;
                for (int i = 0; i < args.frames; ++i) {
                    shell.camera().yawDeg = yaw0 + 360.0f * static_cast<float>(i) / static_cast<float>(args.frames);
                    shell.frame();
                    char name[64];
                    std::snprintf(name, sizeof name, "/frame-%03d.png", i);
                    engine->saveScreenshotPng(args.orbitDir + name);
                }
                std::printf("wrote %d frames to %s\n", args.frames, args.orbitDir.c_str());
            }
            if (!args.tourDir.empty()) {
                auto* strips = dynamic_cast<StripView*>(scene.get());
                if (!strips) throw std::runtime_error("--tour draws bit strips only");
                tour(shell, *engine, *strips, args);
            }
            if (auto* strips = dynamic_cast<StripView*>(scene.get()); strips && strips->hasSelection())
                std::printf("selected %s\n", strips->selectionPath().c_str());
            return 0;
        }
        while (!shell.quitRequested()) {
            StrataEvent e;
            while (engine->pollEvent(e)) shell.handle(e);
            shell.frame();
            engine->present();
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "asn1-viz: %s\n", e.what());
        return 1;
    }
    return 0;
}
