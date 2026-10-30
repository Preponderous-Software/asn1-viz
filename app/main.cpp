// asn1-viz: the desktop viewer.
//
//   asn1-viz [FILE] [options]
//   asn1-viz --compare A.json B.json [options]
//
// FILE is an asn1-layout/1 document (bit strips, RFC 0023), an
// asn1-schema/1 document (the schema city, RFC 0024) or an asn1-compile/1
// trace (the compilation replay over the city, RFC 0034); with no FILE,
// datasets/pkix/certificate.layout.json is opened if it is there.
//
// Options:
//   --pick PATH          strips: select a field by path (e.g. Certificate.tbsCertificate.validity)
//                        city: select a type by name (e.g. Certificate)
//   --schema FILE        strips: an asn1-schema/1 file, for "file:line" in the pick panel
//                        (default: schema.json beside FILE, if there is one)
//   --hops N             city: how far the selection's references are followed (1-4)
//   --no-loops           city: do not draw the recursion loops
//   --at F               compile: start paused at fraction F of the run (0..1)
//   --no-lookups         compile: leave the fixer's lookup events off the timeline
//   --compare A B        two asn1-compile/1 traces of one schema, compared (RFC 0034 G4);
//                        C switches between the comparison and A's replay
//   --pick Type/function compile, compare: select a floor
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
//   --headless --replay DIR [--frames N]   compile: the whole run in N frames, turning slowly
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>

#include "model/layout.h"
#include "view/compare_view.h"
#include "view/compile_view.h"
#include "view/open.h"
#include "view/shell.h"
#include "view/strip_view.h"

using namespace asn1viz;

namespace {

struct Args {
    std::string file, compareB;
    bool headless = false;
    std::string shot, orbitDir, tourDir, replayDir;
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
    std::string schema;
    int hops = 1;
    bool loops = true;
    float at = -1;
    bool noLookups = false;
};

[[noreturn]] void usage(int code) {
    std::fprintf(stderr,
                 "usage: asn1-viz [FILE] [--pick PATH] [--lane N] [--click X Y] [--links D]\n"
                 "                [--row-bits N] [--yaw D] [--pitch D] [--zoom F] [--focus]\n"
                 "                [--at F] [--no-lookups] [--compare A.json B.json]\n"
                 "                [--width W --height H] [--headless (--shot PATH | --orbit DIR | --replay DIR [--frames N])]\n");
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
        else if (s == "--replay") a.replayDir = next();
        else if (s == "--at") a.at = static_cast<float>(std::atof(next()));
        else if (s == "--no-lookups") a.noLookups = true;
        else if (s == "--compare") {
            a.file = next();
            a.compareB = next();
        }
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
        else if (s == "--schema") a.schema = next();
        else if (s == "--hops") a.hops = std::atoi(next());
        else if (s == "--no-loops") a.loops = false;
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
    if (a.headless && a.shot.empty() && a.orbitDir.empty() && a.tourDir.empty() && a.replayDir.empty()) {
        std::fprintf(stderr, "asn1-viz: --headless needs --shot PATH, --orbit DIR, --tour DIR or --replay DIR\n");
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

// The compilation replay, start to end, in a.frames frames: each stage gets
// frames in proportion to its share of the scrubber, the camera turning a
// little all the while, and the last frame is the finished run.
void replay(Shell& shell, strata::Engine& engine, CompileView& view, const Args& a) {
    view.setClock(false);
    const float yaw0 = shell.camera().yawDeg;
    const int n = std::max(2, a.frames);
    for (int i = 0; i < n; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(n - 1);
        view.seekBar(u);
        shell.camera().yawDeg = yaw0 + 40.0f * u;
        shell.frame();
        char name[64];
        std::snprintf(name, sizeof name, "/frame-%03d.png", i);
        engine.saveScreenshotPng(a.replayDir + name);
    }
    std::printf("wrote %d frames (%d steps) to %s\n", n, view.replay().steps(), a.replayDir.c_str());
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
        OpenOptions oo;
        oo.pick = args.pick;
        oo.lane = args.lane;
        oo.links = args.links;
        oo.rowBits = args.rowBits;
        oo.schema = args.schema;
        oo.hops = args.hops;
        oo.loops = args.loops;
        oo.compact = args.width < 1000;
        // Headless renders hold the playhead still (at the start, if no --at).
        oo.at = args.headless && args.at < 0 ? 0 : args.at;
        oo.hideLookups = args.noLookups;
        std::unique_ptr<Scene> scene =
            args.compareB.empty() ? openScene(args.file, oo) : openCompare(args.file, args.compareB, oo);
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
            if (!args.replayDir.empty()) {
                auto* compile = replayOf(*scene);
                if (!compile) throw std::runtime_error("--replay draws an asn1-compile/1 trace only");
                replay(shell, *engine, *compile, args);
            }
            if (const auto* pair = dynamic_cast<const CompilePair*>(scene.get()); pair && pair->comparing()) {
                const CompileCompare& c = pair->comparison().compare();
                std::printf("compare %s -> %s: %d types changed, %d lines -> %d, fixer differs on %d types\n",
                            CompileCompare::flagsLabel(c.a()).c_str(), CompileCompare::flagsLabel(c.b()).c_str(),
                            c.typesChanged(), c.totalA(), c.totalB(), c.typesFixerDiffers());
            }
            if (auto* compile = replayOf(*scene))
                std::printf("stage %s, step %d of %d\n", stageName(compile->replay().stage()),
                            static_cast<int>(compile->replay().position()), compile->replay().steps());
            const std::string selected = selectionOf(*scene);
            if (!selected.empty()) std::printf("selected %s\n", selected.c_str());
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
