#include "scene/camera.h"

#include <cmath>

namespace asn1viz {

namespace {

constexpr float kPi = 3.14159265358979f;

Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 normalize(Vec3 a) {
    const float l = std::sqrt(dot(a, a));
    return l > 0 ? Vec3{a.x / l, a.y / l, a.z / l} : a;
}

void mul4(const float m[16], const float v[4], float out[4]) {
    for (int r = 0; r < 4; ++r) out[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] * v[3];
}

}  // namespace

Vec3 OrbitCamera::eye() const {
    const float yaw = yawDeg * kPi / 180, pitch = pitchDeg * kPi / 180;
    const float c = std::cos(pitch);
    return {target.x + distance * c * std::sin(yaw), target.y + distance * std::sin(pitch),
            target.z + distance * c * std::cos(yaw)};
}

CameraMatrices matrices(const OrbitCamera& cam, float aspect) {
    CameraMatrices m{};
    const Vec3 eye = cam.eye();
    m.eye = eye;
    const Vec3 f = normalize(sub(cam.target, eye));
    Vec3 up{0, 1, 0};
    if (std::fabs(dot(f, up)) > 0.999f) up = {0, 0, -1};
    const Vec3 s = normalize(cross(f, up));
    const Vec3 u = cross(s, f);
    const float view[16] = {s.x, u.x, -f.x, 0, s.y, u.y, -f.y, 0, s.z, u.z, -f.z, 0,
                            -dot(s, eye), -dot(u, eye), dot(f, eye), 1};
    for (int i = 0; i < 16; ++i) m.view[i] = view[i];
    const float t = 1.0f / std::tan(cam.fovYDeg * kPi / 360);
    const float n = cam.nearPlane, fa = cam.farPlane;
    for (float& v : m.projection) v = 0;
    m.projection[0] = t / aspect;
    m.projection[5] = t;
    m.projection[10] = (fa + n) / (n - fa);
    m.projection[11] = -1;
    m.projection[14] = 2 * fa * n / (n - fa);
    return m;
}

bool toScreen(const CameraMatrices& m, Vec3 p, int width, int height, float& x, float& y) {
    const float in[4] = {p.x, p.y, p.z, 1};
    float v[4], c[4];
    mul4(m.view, in, v);
    mul4(m.projection, v, c);
    if (c[3] <= 0.01f) return false;
    x = (c[0] / c[3] * 0.5f + 0.5f) * static_cast<float>(width);
    y = (0.5f - c[1] / c[3] * 0.5f) * static_cast<float>(height);
    return x >= 0 && x <= static_cast<float>(width) && y >= 0 && y <= static_cast<float>(height);
}

bool invert4(const float m[16], float inv[16]) {
    float t[16];
    t[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
           m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    t[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
           m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    t[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
           m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    t[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
            m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    t[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
           m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    t[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
           m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    t[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
           m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    t[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
            m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    t[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
           m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    t[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
           m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    t[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
            m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    t[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
            m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    t[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
           m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    t[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
           m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    t[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
            m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    t[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
            m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const float det = m[0] * t[0] + m[1] * t[4] + m[2] * t[8] + m[3] * t[12];
    if (std::fabs(det) < 1e-20f) return false;
    for (int i = 0; i < 16; ++i) inv[i] = t[i] / det;
    return true;
}

void rayThroughPixel(const CameraMatrices& m, float px, float py, int width, int height, Vec3& origin,
                     Vec3& direction) {
    // clip = P * V * world, so world = (P * V)^-1 * clip.
    float pv[16];
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += m.projection[k * 4 + r] * m.view[c * 4 + k];
            pv[c * 4 + r] = s;
        }
    float inv[16];
    origin = m.eye;
    if (!invert4(pv, inv)) {
        direction = {0, 0, -1};
        return;
    }
    const float nx = 2 * px / static_cast<float>(width) - 1;
    const float ny = 1 - 2 * py / static_cast<float>(height);
    const float clip[4] = {nx, ny, 1, 1};
    float w[4];
    mul4(inv, clip, w);
    const Vec3 far{w[0] / w[3], w[1] / w[3], w[2] / w[3]};
    direction = normalize(sub(far, m.eye));
}

}  // namespace asn1viz
