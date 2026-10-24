#include <cmath>

#include "check.h"
#include "scene/camera.h"

using namespace asn1viz;

int main() {
    OrbitCamera cam;
    cam.target = {10, 0, -5};
    cam.distance = 40;
    const CameraMatrices m = matrices(cam, 16.0f / 9.0f);
    const int w = 1280, h = 720;
    // The target is in the middle of the screen.
    float x = 0, y = 0;
    CHECK(toScreen(m, cam.target, w, h, x, y));
    CHECK(std::fabs(x - w / 2.0f) < 0.5f && std::fabs(y - h / 2.0f) < 0.5f);
    // A ray through any pixel passes through the point that projects there.
    const Vec3 p{14, 3, -2};
    CHECK(toScreen(m, p, w, h, x, y));
    Vec3 o, d;
    rayThroughPixel(m, x, y, w, h, o, d);
    const Vec3 op{p.x - o.x, p.y - o.y, p.z - o.z};
    const float t = op.x * d.x + op.y * d.y + op.z * d.z;
    const Vec3 closest{o.x + d.x * t, o.y + d.y * t, o.z + d.z * t};
    const float miss = std::sqrt((closest.x - p.x) * (closest.x - p.x) + (closest.y - p.y) * (closest.y - p.y) +
                                 (closest.z - p.z) * (closest.z - p.z));
    CHECK(miss < 0.01f);
    CHECK(std::fabs(std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) - 1) < 1e-4f);
    // A point behind the eye is not on screen.
    const Vec3 e = cam.eye();
    const Vec3 behind{e.x + (e.x - cam.target.x), e.y + (e.y - cam.target.y), e.z + (e.z - cam.target.z)};
    CHECK(!toScreen(m, behind, w, h, x, y));
    // invert4 round trip.
    float inv[16], back[16];
    CHECK(invert4(m.view, inv));
    CHECK(invert4(inv, back));
    bool same = true;
    for (int i = 0; i < 16; ++i) same = same && std::fabs(back[i] - m.view[i]) < 1e-3f;
    CHECK(same);
    TEST_MAIN_END();
}
