// asn1-viz: the desktop viewer.
//
//   asn1-viz [--headless --shot PATH] [--width W --height H]
//
// The foundation build draws a test pattern (the bits of the text "ASN.1",
// one box per bit, ones raised) through the shared viewer shell, so the
// engine, picking and screenshots can be checked before any view exists.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "view/shell.h"

using namespace asn1viz;

namespace {

struct Args {
    bool headless = false;
    std::string shot;
    int width = 1280, height = 720;
};

class TestPattern : public Scene {
public:
    void upload(strata::Engine& engine) override {
        MeshBuilder m;
        const char* text = "ASN.1";
        for (int i = 0; text[i]; ++i)
            for (int b = 0; b < 8; ++b) {
                const int bit = (static_cast<unsigned char>(text[i]) >> (7 - b)) & 1;
                const float x = static_cast<float>(i * 8 + b) * 1.1f + static_cast<float>(i) * 0.6f;
                const float lo[3] = {x, 0, 0}, hi[3] = {x + 1, bit ? 2.0f : 0.6f, 1};
                const Rgb c = bit ? Rgb{0.95f, 0.62f, 0.2f} : Rgb{0.25f, 0.4f, 0.6f};
                m.addBox(lo, hi, c, c, i * 8 + b);
            }
        tags_ = m.tags();
        mesh_ = uploadMesh(engine, m);
    }
    OrbitCamera home(float) const override {
        OrbitCamera c;
        c.target = {23, 0, 0.5f};
        c.distance = 42;
        c.yawDeg = -20;
        c.pitchDeg = 30;
        return c;
    }
    void pick(const StrataRayHit& hit) override {
        picked_ = hit.mesh_id == mesh_ ? tagForTriangle(tags_, hit.triangle) : -1;
    }
    void draw(Overlay& o, const CameraMatrices& m, int w, int h) override {
        const float ink[4] = {0.9f, 0.92f, 0.95f, 1};
        o.text(16, 16, 16, "asn1-viz test pattern: the bits of \"ASN.1\"", ink);
        if (picked_ >= 0) {
            const std::string s = "picked bit " + std::to_string(picked_);
            o.text(16, 40, 16, s, ink);
            float x, y;
            const float bx = static_cast<float>(picked_) * 1.1f + static_cast<float>(picked_ / 8) * 0.6f + 0.5f;
            if (toScreen(m, {bx, 2.4f, 0.5f}, w, h, x, y)) o.text(x - 8, y - 24, 16, "v", ink);
        }
    }
    int picked() const { return picked_; }

private:
    uint32_t mesh_ = 0;
    std::vector<TriTag> tags_;
    int picked_ = -1;
};

bool parse(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "asn1-viz: %s needs a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (s == "--headless") a.headless = true;
        else if (s == "--shot") a.shot = next("--shot");
        else if (s == "--width") a.width = std::atoi(next("--width"));
        else if (s == "--height") a.height = std::atoi(next("--height"));
        else if (s == "--version") {
            std::printf("asn1-viz %s\n", ASN1VIZ_VERSION);
            std::exit(0);
        } else {
            std::fprintf(stderr, "asn1-viz: unknown argument %s\n", s.c_str());
            return false;
        }
    }
    if (a.headless && a.shot.empty()) {
        std::fprintf(stderr, "asn1-viz: --headless needs --shot PATH\n");
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse(argc, argv, args)) {
        std::fprintf(stderr, "usage: asn1-viz [--headless --shot PATH] [--width W --height H]\n");
        return 2;
    }
    try {
        strata::EngineDesc desc;
        desc.surface = args.headless ? STRATA_SURFACE_HEADLESS : STRATA_SURFACE_WINDOW;
        desc.width = args.width;
        desc.height = args.height;
        desc.title = "asn1-viz";
        auto engine = strata::Engine::create(desc);
        TestPattern scene;
        Shell shell(*engine, scene);
        if (args.headless) {
            shell.pickAt(static_cast<float>(args.width) / 2, static_cast<float>(args.height) / 2);
            shell.frame();
            engine->saveScreenshotPng(args.shot);
            std::printf("picked bit %d; wrote %s\n", scene.picked(), args.shot.c_str());
            return scene.picked() >= 0 ? 0 : 1;
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
