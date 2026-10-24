// The strip geometry: one box per bit, tagged with its segment, placed in
// rows; cross-lane matching by path.
#include <string>

#include "check.h"
#include "model/layout.h"
#include "scene/strip.h"

using namespace asn1viz;

int main(int argc, char** argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    const Layout l = loadLayoutFile(root + "/datasets/demo/msg.layout.json");
    StripStyle style;
    style.rowBits = 32;
    const StripScene s(l, style);
    CHECK(s.laneCount() == 2);
    for (size_t lane = 0; lane < s.laneCount(); ++lane) {
        const MeshBuilder& m = s.laneMesh(lane);
        CHECK(m.vertexCount() == static_cast<int32_t>(l.lanes[lane].bits) * 24);
        // Each bit's 12 triangles name the segment that holds the bit.
        bool tagsRight = true;
        for (int64_t b = 0; b < l.lanes[lane].bits; ++b)
            tagsRight = tagsRight && tagForTriangle(m.tags(), static_cast<int32_t>(b * 12 + 5)) == l.lanes[lane].segmentAt(b);
        CHECK(tagsRight);
    }
    // Rows wrap at 32 bits; lanes sit side by side.
    const BitPlace p0 = s.place(0, 0), p32 = s.place(0, 32), q0 = s.place(1, 0);
    CHECK(p32.x0 == p0.x0 && p32.z0 > p0.z0);
    CHECK(q0.x0 > s.place(0, 31).x1);
    CHECK(s.rowSpans(0, 30, 4).size() == 2);  // a range across a row break is two boxes
    // The same field in the other lane.
    const int f = l.lanes[0].frameByPath("Msg.extra");
    const int g = s.matchingFrame(0, f, 1);
    CHECK(g >= 0 && l.lanes[1].frames[g].path == "Msg.extra");
    // Deeper fields stand taller.
    const int deep = l.lanes[0].frameByPath("Msg.items[1].b");
    CHECK(s.frameHeight(0, deep) > s.frameHeight(0, 0));
    // The home camera sees both lanes.
    const OrbitCamera c = s.homeCamera(16.0f / 9.0f);
    const CameraMatrices m = matrices(c, 16.0f / 9.0f);
    float x, y;
    CHECK(toScreen(m, s.anchor(0, 0, 0), 1280, 720, x, y));
    CHECK(toScreen(m, s.anchor(1, l.lanes[1].bits - 1, 0), 1280, 720, x, y));
    // Every kind has a colour that is not the fallback magenta.
    for (const LayoutLane& lane : l.lanes)
        for (const LayoutSegment& seg : lane.segments) {
            const Rgb c2 = kindColor(seg.kind, "");
            CHECK(!(c2.r == 1 && c2.g == 0 && c2.b == 1));
        }
    TEST_MAIN_END();
}
