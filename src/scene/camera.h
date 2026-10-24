// An orbit camera and the two projections a picking viewer needs: a world
// point to the screen (for labels, as Atman's view does) and a screen pixel
// to a world ray (for strata_raycast_meshes). No engine: matrices are
// column-major float[16], the layout StrataCamera uses, with a right-handed
// look-at view and an OpenGL (depth -1..1) projection.
#pragma once

namespace asn1viz {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

struct OrbitCamera {
    Vec3 target;
    float yawDeg = -30;    // about +Y; 0 looks along -Z
    float pitchDeg = 35;   // above the horizon
    float distance = 50;
    float fovYDeg = 45;
    float nearPlane = 0.1f;
    float farPlane = 5000;

    Vec3 eye() const;
};

struct CameraMatrices {
    float view[16];
    float projection[16];
    Vec3 eye;
};

CameraMatrices matrices(const OrbitCamera& cam, float aspect);

// Where a world point lands on a width x height screen, in pixels from the
// top-left; false if it is behind the eye or off the screen.
bool toScreen(const CameraMatrices& m, Vec3 p, int width, int height, float& x, float& y);

// The world-space ray through pixel (px, py): origin at the eye, direction
// normalised.
void rayThroughPixel(const CameraMatrices& m, float px, float py, int width, int height, Vec3& origin,
                     Vec3& direction);

// General 4x4 inverse (column-major); false when singular.
bool invert4(const float m[16], float out[16]);

}  // namespace asn1viz
