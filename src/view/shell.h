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

    void line(Vec3 a, Vec3 b, const float rgba[4]);
    void text(float x, float y, float pixelSize, const std::string& s, const float rgba[4], bool shadow = true);
    void clear();
};

// Uploads a MeshBuilder to the engine and returns the mesh id (0 if empty).
uint32_t uploadMesh(strata::Engine& engine, const MeshBuilder& mesh);

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
    // A key press the shell did not use; true if the scene did.
    virtual bool key(int32_t scancode) {
        (void)scancode;
        return false;
    }
    // Where the camera should look when something was just picked; false
    // to leave it.
    virtual bool focus(Vec3& target) const {
        (void)target;
        return false;
    }
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
    bool quit_ = false;
    int dragButton_ = 0;
    float dragDistance_ = 0;
    float lastX_ = 0, lastY_ = 0;
};

}  // namespace asn1viz
