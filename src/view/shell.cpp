#include "view/shell.h"

#include <algorithm>
#include <cmath>

namespace asn1viz {

namespace {

// SDL3 scancodes (USB HID usage ids), as StrataEvent carries them.
constexpr int32_t kScanEscape = 41;
constexpr int32_t kScanHome = 74;
constexpr int32_t kScanH = 11;
constexpr int32_t kScanF = 9;

}  // namespace

void Overlay::line(Vec3 a, Vec3 b, const float rgba[4]) {
    StrataLineVertex v{};
    v.position[0] = a.x;
    v.position[1] = a.y;
    v.position[2] = a.z;
    std::copy(rgba, rgba + 4, v.color);
    lines.push_back(v);
    v.position[0] = b.x;
    v.position[1] = b.y;
    v.position[2] = b.z;
    lines.push_back(v);
}

void Overlay::text(float x, float y, float pixelSize, const std::string& s, const float rgba[4], bool shadow) {
    strings.push_back(s);
    StrataText t{};
    t.x = std::round(x);
    t.y = std::round(y);
    t.pixel_size = pixelSize * ui;
    t.flags = shadow ? STRATA_TEXT_SHADOW : 0u;
    std::copy(rgba, rgba + 4, t.color);
    t.utf8 = strings.back().c_str();
    texts.push_back(t);
}

void Overlay::clear() {
    lines.clear();
    texts.clear();
    strings.clear();
}

uint32_t uploadMesh(strata::Engine& engine, const MeshBuilder& mesh, bool unlit) {
    if (mesh.empty()) return 0;
    StrataMesh m{};
    m.positions = mesh.positions().data();
    m.normals = mesh.normals().data();
    m.colors = mesh.colors().data();
    m.indices = mesh.indices().data();
    m.vertex_count = mesh.vertexCount();
    m.index_count = static_cast<int32_t>(mesh.indices().size());
    m.color[0] = m.color[1] = m.color[2] = m.color[3] = 1;
    if (unlit) {
        StrataTexturedMesh t{};
        t.mesh = m;
        t.texture = 0;
        t.flags = STRATA_MESH_UNLIT;
        return engine.addTexturedMesh(t);
    }
    return engine.addMesh(m);
}

Shell::Shell(strata::Engine& engine, Scene& scene) : engine_(engine), scene_(scene) {
    engine_.drawableSize(width_, height_);
    StrataLighting light = strata::lightingDefaults(STRATA_LIGHTING_SUN);
    light.time_of_day = 0.42f;
    light.latitude_degrees = 35;
    light.cascade_count = 3;
    light.shadow_map_size = 2048;
    light.shadow_distance = 400;
    light.fog = 0;
    engine_.setLighting(light);
    scene_.upload(engine_);
    home();
}

void Shell::home() {
    camera_ = scene_.home(height_ > 0 ? static_cast<float>(width_) / static_cast<float>(height_) : 16.0f / 9.0f);
}

void Shell::resize(int32_t width, int32_t height) {
    if (width < 1 || height < 1) return;
    engine_.resize(width, height);
    engine_.drawableSize(width_, height_);
}

void Shell::lookAtFocus() {
    Vec3 t;
    if (scene_.focus(t)) camera_.target = t;
}

void Shell::pickAt(float px, float py) {
    const CameraMatrices m = matrices(camera_, static_cast<float>(width_) / static_cast<float>(std::max(1, height_)));
    Vec3 o, d;
    rayThroughPixel(m, px, py, width_, height_, o, d);
    const float origin[3] = {o.x, o.y, o.z}, dir[3] = {d.x, d.y, d.z};
    scene_.pick(engine_.raycastMeshes(origin, dir, camera_.farPlane));
}

void Shell::handle(const StrataEvent& e) {
    switch (e.type) {
        case STRATA_EVENT_QUIT: quit_ = true; break;
        case STRATA_EVENT_RESIZED:
            // The window changed (or was changed by resize()): follow it, without
            // resizing again (a high-density canvas reports more pixels than asked).
            engine_.drawableSize(width_, height_);
            break;
        case STRATA_EVENT_MOUSE_BUTTON_DOWN:
            sceneDrag_ = e.button == 1 && scene_.press(e.x * pointerX_, e.y * pointerY_, false);
            dragButton_ = sceneDrag_ ? 0 : e.button;
            dragDistance_ = 0;
            lastX_ = e.x;
            lastY_ = e.y;
            break;
        case STRATA_EVENT_MOUSE_BUTTON_UP:
            if (sceneDrag_) {
                sceneDrag_ = false;
                break;
            }
            if (e.button == 1 && dragDistance_ < 4) pickAt(e.x * pointerX_, e.y * pointerY_);  // a click, not a drag
            dragButton_ = 0;
            break;
        case STRATA_EVENT_MOUSE_MOTION: {
            const float dx = e.x - lastX_, dy = e.y - lastY_;
            lastX_ = e.x;
            lastY_ = e.y;
            if (sceneDrag_) {
                scene_.press(e.x * pointerX_, e.y * pointerY_, true);
                break;
            }
            if (!dragButton_) break;
            dragDistance_ += std::fabs(dx) + std::fabs(dy);
            if (dragButton_ == 1) {
                camera_.yawDeg -= dx * 0.3f;
                camera_.pitchDeg = std::clamp(camera_.pitchDeg + dy * 0.3f, 5.0f, 89.0f);
            } else {
                // Pan in the ground plane, scaled by the distance so it tracks the cursor.
                const float k = camera_.distance * 0.0015f;
                const float yaw = camera_.yawDeg * 3.14159265f / 180;
                camera_.target.x += (-std::cos(yaw) * dx - std::sin(yaw) * dy) * k;
                camera_.target.z += (std::sin(yaw) * dx - std::cos(yaw) * dy) * k;
            }
            break;
        }
        case STRATA_EVENT_MOUSE_WHEEL:
            camera_.distance = std::clamp(camera_.distance * std::pow(0.88f, e.y), 2.0f, 4000.0f);
            break;
        case STRATA_EVENT_KEY_DOWN:
            if (e.scancode == kScanEscape) quit_ = true;
            else if (e.scancode == kScanHome || e.scancode == kScanH) home();
            else if (scene_.key(e.scancode) && e.scancode == kScanF) lookAtFocus();
            break;
        default: break;
    }
}

void Shell::frame() {
    const float aspect = static_cast<float>(width_) / static_cast<float>(std::max(1, height_));
    const CameraMatrices m = matrices(camera_, aspect);
    StrataCamera cam{};
    std::copy(m.view, m.view + 16, cam.view);
    std::copy(m.projection, m.projection + 16, cam.projection);
    cam.eye[0] = m.eye.x;
    cam.eye[1] = m.eye.y;
    cam.eye[2] = m.eye.z;
    cam.near_plane = camera_.nearPlane;
    cam.far_plane = camera_.farPlane;
    engine_.setCamera(cam);

    scene_.sync(engine_);
    overlay_.clear();
    overlay_.ui = ui_;
    scene_.draw(overlay_, m, width_, height_);

    StrataFrameDesc fd{};
    fd.clear_flags = STRATA_CLEAR_COLOR | STRATA_CLEAR_DEPTH;
    const float bg[4] = {0.035f, 0.045f, 0.07f, 1};
    std::copy(bg, bg + 4, fd.clear_color);
    engine_.beginFrame(fd);
    if (!overlay_.lines.empty()) engine_.submitLines(overlay_.lines.data(), overlay_.lines.size());
    if (!overlay_.texts.empty()) engine_.submitText(overlay_.texts.data(), overlay_.texts.size());
    engine_.endFrame();
}

}  // namespace asn1viz
