// The viewer shell both asn1-viz views share (RFC 0023 section 4, RFC 0024
// section 5): the Strata engine, an orbit camera driven by the mouse, ray
// picking through strata_raycast_meshes, screen-space labels projected from
// world points, and headless screenshots.
//
// A view (the bit strips, the schema city) is a Scene: it uploads its meshes
// once, answers picks, and draws its overlays (lines and text) each frame.
#pragma once

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "scene/camera.h"
#include "scene/mesh.h"
#include "strata/strata.hpp"

namespace asn1viz {

// This frame's overlays: world-space lines and screen-space text.
struct Overlay {
    std::vector<StrataLineVertex> lines;
    std::vector<StrataText> texts;
    std::deque<std::string> strings;  // owns the text the StrataText entries point to
    float ui = 1;                     // text and screen-space spacing scale (a dense canvas draws bigger)

    void line(Vec3 a, Vec3 b, const float rgba[4]);
    void text(float x, float y, float pixelSize, const std::string& s, const float rgba[4], bool shadow = true);
    void clear();
};

// One line of a view's pick panel (drawn on the canvas, or by a page).
struct PanelLine {
    std::string text;
    float color[4];
    float size;  // 16 or 8 (font pixels, before the ui scale)
};

// Uploads a MeshBuilder to the engine and returns the mesh id (0 if empty).
// `unlit` draws it without lighting or shadows (a glow, ABI 1.7).
uint32_t uploadMesh(strata::Engine& engine, const MeshBuilder& mesh, bool unlit = false);

class Scene {
public:
    virtual ~Scene() = default;
    // Adds the scene's meshes to the engine (called once, and again after
    // the engine's meshes are cleared).
    virtual void upload(strata::Engine& engine) = 0;
    // The camera the view starts from and returns to (Home).
    virtual OrbitCamera home(float aspect) const = 0;
    // A ray hit from a click (hit.mesh_id 0: the click hit nothing).
    virtual void pick(const StrataRayHit& hit) = 0;
    // This frame's lines and labels.
    virtual void draw(Overlay& overlay, const CameraMatrices& camera, int width, int height) = 0;
    // Called before each frame: a scene whose meshes depend on the selection
    // (a highlight) updates them here.
    virtual void sync(strata::Engine& engine) { (void)engine; }
    // A key press the shell did not use; true if the scene did.
    virtual bool key(int32_t scancode) {
        (void)scancode;
        return false;
    }
    // A press (or, while held, a drag) at a drawable pixel. True if the
    // scene used it for its own on-canvas control (the compilation view's
    // scrubber): the shell then neither orbits nor picks until the release.
    virtual bool press(float px, float py, bool dragging) {
        (void)px;
        (void)py;
        (void)dragging;
        return false;
    }
    // The pick panel's lines; `fit` is the most characters a line may hold.
    virtual std::vector<PanelLine> panel(size_t fit) const {
        (void)fit;
        return {};
    }
    // Draws the panel on the canvas (false: a page shows it instead).
    void setPanelOnCanvas(bool on) { panelOnCanvas_ = on; }
    bool panelOnCanvas() const { return panelOnCanvas_; }
    // Where the camera should look when something was just picked; false
    // to leave it.
    virtual bool focus(Vec3& target) const {
        (void)target;
        return false;
    }

private:
    bool panelOnCanvas_ = true;
};

class Shell {
public:
    Shell(strata::Engine& engine, Scene& scene);

    // Handles one input event (orbit, pan, zoom, pick, keys).
    void handle(const StrataEvent& e);
    // Draws one frame (does not present).
    void frame();
    bool quitRequested() const { return quit_; }

    void resize(int32_t width, int32_t height);
    // Pointer events arrive in window units; the drawable can be larger (a
    // high-density browser canvas). Drawable pixels per window unit.
    void setPointerScale(float sx, float sy) {
        pointerX_ = sx;
        pointerY_ = sy;
    }
    // Scales on-canvas text and its spacing (for a dense canvas).
    void setUiScale(float ui) { ui_ = ui > 0 ? ui : 1; }
    // Picks what is under a pixel, as a click there would.
    void pickAt(float px, float py);
    void home();
    OrbitCamera& camera() { return camera_; }
    // Points the camera at the scene's focus, keeping its angles.
    void lookAtFocus();

private:
    strata::Engine& engine_;
    Scene& scene_;
    OrbitCamera camera_;
    Overlay overlay_;
    int32_t width_ = 0, height_ = 0;
    float pointerX_ = 1, pointerY_ = 1;
    float ui_ = 1;
    bool quit_ = false;
    int dragButton_ = 0;
    bool sceneDrag_ = false;  // a press the scene took (its scrubber)
    float dragDistance_ = 0;
    float lastX_ = 0, lastY_ = 0;
};

}  // namespace asn1viz
