#include "scene/compile_city.h"

#include <algorithm>
#include <cmath>

namespace asn1viz {

namespace {

constexpr int32_t kBandBase = 1 << 20;
constexpr int32_t kSkeletonBase = 1 << 24;
constexpr int32_t kSharedBase = 1 << 25;
constexpr int kMaxFns = 128;  // tag room per type (the trace has 26 on PKIX)

constexpr float kFootprint = 0.04f;  // an assignment not parsed yet
constexpr float kLibraryGap = 7.0f;  // between the city and the library
// Skeletons are many (hundreds) and small: a tighter grid than buildings.
constexpr float kBook = 0.5f, kBookGap = 0.2f, kShelfPad = 1.2f;

Rgb hsv(float h, float s, float v) {
    h = h - std::floor(h);
    const float c = v * s, x = c * (1 - std::fabs(std::fmod(h * 6, 2.0f) - 1)), m = v - c;
    float r = 0, g = 0, b = 0;
    switch (static_cast<int>(h * 6) % 6) {
        case 0: r = c, g = x; break;
        case 1: r = x, g = c; break;
        case 2: g = c, b = x; break;
        case 3: g = x, b = c; break;
        case 4: r = x, b = c; break;
        default: r = c, b = x; break;
    }
    return {r + m, g + m, b + m};
}

Rgb scale(Rgb c, float k) { return {std::min(1.0f, c.r * k), std::min(1.0f, c.g * k), std::min(1.0f, c.b * k)}; }

float smooth(float t) { return t * t * (3 - 2 * t); }

}  // namespace

int32_t bandTag(int type, int fn) { return kBandBase + type * kMaxFns + std::clamp(fn, 0, kMaxFns - 1); }
int32_t sharedTag(int file, int fn) { return kSharedBase + file * kMaxFns + std::clamp(fn, 0, kMaxFns - 1); }
int32_t skeletonTag(int index) { return kSkeletonBase + index; }

CompilePick decodeTag(int32_t tag) {
    CompilePick p;
    if (tag < 0) return p;
    if (tag >= kSharedBase) {
        p.kind = CompilePick::SharedBand;
        p.file = (tag - kSharedBase) / kMaxFns;
        p.fn = (tag - kSharedBase) % kMaxFns;
    } else if (tag >= kSkeletonBase) {
        p.kind = CompilePick::Skeleton;
        p.skeleton = tag - kSkeletonBase;
    } else if (tag >= kBandBase) {
        p.kind = CompilePick::Band;
        p.type = (tag - kBandBase) / kMaxFns;
        p.fn = (tag - kBandBase) % kMaxFns;
    } else {
        p.kind = CompilePick::Type;
        p.type = tag;
    }
    return p;
}

Rgb functionColor(int fn, int functionCount) {
    if (fn >= functionCount || fn < 0) return {0.42f, 0.45f, 0.52f};  // unannotated: includes, headers
    return hsv(0.07f + 0.618034f * static_cast<float>(fn), 0.58f, 0.95f);
}

Rgb familyColor(PassFamily f) {
    switch (f) {
        case PassFamily::Dereference: return {0.3f, 0.85f, 1.0f};
        case PassFamily::Tagging: return {0.72f, 0.5f, 1.0f};
        case PassFamily::Constraints: return {1.0f, 0.58f, 0.2f};
        case PassFamily::Values: return {1.0f, 0.9f, 0.3f};
        case PassFamily::Classes: return {1.0f, 0.38f, 0.35f};
        case PassFamily::Lookup: return {0.55f, 0.6f, 0.75f};
        case PassFamily::Other: return {0.45f, 0.95f, 0.5f};
    }
    return {1, 1, 1};
}

CompileCity::CompileCity(const Schema& schema, const CompileTrace& trace, CityStyle style)
    : city_(schema, style), replay_(trace, static_cast<int>(schema.types.size())), style_(style) {
    const int n = static_cast<int>(schema.types.size());
    functions_ = trace.functions;
    for (const CompileFile& f : trace.files)
        for (const auto& kv : f.byFn)
            if (functionIndex(kv.first) < 0) functions_.push_back(kv.first);
    filesOf_.resize(static_cast<size_t>(n));
    std::vector<int> total(static_cast<size_t>(n), 0);
    int most = 1;
    for (size_t f = 0; f < trace.files.size(); ++f) {
        const CompileFile& file = trace.files[f];
        if (file.type >= 0 && file.type < n) {
            filesOf_[static_cast<size_t>(file.type)].push_back(static_cast<int>(f));
            total[static_cast<size_t>(file.type)] += file.lines;
            most = std::max(most, total[static_cast<size_t>(file.type)]);
        } else {
            sharedFiles_.push_back(static_cast<int>(f));
            most = std::max(most, file.lines);
        }
    }
    // The biggest type's code stands about twice as tall as the city's tallest building.
    const float target = std::max(6.0f, city_.tallest() * 2.2f);
    lineScale_ = target / static_cast<float>(most);
    tallestStack_ = target;

    // The library, beside the city on +X: shared files, then skeletons.
    float x1 = -1e9f, z0 = 1e9f;
    for (const District& d : city_.districts()) {
        x1 = std::max(x1, d.x1);
        z0 = std::min(z0, d.z0);
    }
    if (city_.districts().empty()) x1 = z0 = 0;
    const float cell = style.footprint + style.gap;
    auto shelf = [&](size_t count, float zStart, float step, float gap, float pad) {
        Shelf s{};
        const int items = std::max<int>(1, static_cast<int>(count));
        s.cols = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<float>(items) * 0.8f))));
        const int rows = (items + s.cols - 1) / s.cols;
        s.x0 = x1 + kLibraryGap;
        s.z0 = zStart;
        s.x1 = s.x0 + static_cast<float>(s.cols) * step - gap + 2 * pad;
        s.z1 = s.z0 + static_cast<float>(rows) * step - gap + 2 * pad;
        return s;
    };
    shared_ = shelf(sharedFiles_.size(), z0, cell, style.gap, style.pad);
    skeletons_ = shelf(trace.skeletons.size(), sharedFiles_.empty() ? z0 : shared_.z1 + 3, kBook + kBookGap, kBookGap,
                       kShelfPad);
    if (sharedFiles_.empty()) shared_.x1 = shared_.x0, shared_.z1 = shared_.z0;
}

int CompileCity::functionIndex(const std::string& fn) const {
    for (size_t i = 0; i < functions_.size(); ++i)
        if (functions_[i] == fn) return static_cast<int>(i);
    return -1;
}

std::vector<int> CompileCity::emitted(int type) const {
    const CompileTrace& t = trace();
    std::vector<int> lines(functions_.size() + 1, 0);
    if (type < 0 || type >= static_cast<int>(filesOf_.size())) return lines;
    const int done = replay_.filesDone();
    for (int f : filesOf_[static_cast<size_t>(type)]) {
        if (f >= done) break;
        const CompileFile& file = t.files[static_cast<size_t>(f)];
        for (const auto& kv : file.byFn) {
            const int i = functionIndex(kv.first);
            if (i >= 0) lines[static_cast<size_t>(i)] += kv.second;
        }
        lines.back() += file.unannotated;
    }
    return lines;
}

int CompileCity::emittedTotal(int type) const {
    int n = 0;
    for (int l : emitted(type)) n += l;
    return n;
}

float CompileCity::height(int type) const {
    const Lot& lot = city_.lot(type);
    if (replay_.position() < static_cast<float>(replay_.stageStart(Stage::Fix))) {
        const float r = replay_.rise(type);
        return r <= 0 ? kFootprint : std::max(kFootprint, lot.height * smooth(r));
    }
    const int lines = emittedTotal(type);
    if (lines > 0) return std::max(0.08f, static_cast<float>(lines) * lineScale_);
    return lot.height;
}

long long CompileCity::geometryKey() const {
    const float parseEnd = static_cast<float>(replay_.stageStart(Stage::Fix));
    const long long parse = static_cast<long long>(std::floor(std::min(replay_.position(), parseEnd) * 24));
    return parse * 100000000LL + static_cast<long long>(replay_.filesDone()) * 10000LL + replay_.skeletonsDone();
}

void CompileCity::addStack(MeshBuilder& mesh, float x0, float z0, float x1, float z1, const std::vector<int>& lines,
                           int32_t (*tagOf)(int, int), int owner) const {
    const int fns = static_cast<int>(functions_.size());
    float y = 0;
    // Unannotated lines (includes, headers) at the bottom, then each function.
    for (int k = -1; k < fns; ++k) {
        const int fn = k < 0 ? fns : k;
        const int n = lines[static_cast<size_t>(fn)];
        if (n <= 0) continue;
        const float h = std::max(0.02f, static_cast<float>(n) * lineScale_);
        const Rgb c = functionColor(fn, fns);
        const float lo[3] = {x0, y, z0}, hi[3] = {x1, y + h, z1};
        mesh.addBox(lo, hi, c, scale(c, 1.12f), tagOf(owner, fn));
        y += h;
    }
}

void CompileCity::buildDistrict(size_t di, MeshBuilder& mesh) const {
    const District& d = city_.districts()[di];
    mesh.addTile(d.x0, d.z0, d.x1, d.z1, 0.0f, {30.0f / 255, 34.0f / 255, 46.0f / 255}, -1);
    const bool parsing = replay_.position() < static_cast<float>(replay_.stageStart(Stage::Fix));
    for (int t : d.types) {
        const Lot& lot = city_.lot(t);
        const SchemaType& ty = schema().types[static_cast<size_t>(t)];
        const std::vector<int> lines = emitted(t);
        int total = 0;
        for (int l : lines) total += l;
        if (!parsing && total > 0) {
            addStack(mesh, lot.x0, lot.z0, lot.x1, lot.z1, lines, bandTag, t);
            continue;
        }
        Rgb side = typeKindColor(ty.kind);
        if (parsing && replay_.rise(t) <= 0) side = {0.2f, 0.22f, 0.28f};  // a footprint, waiting for the parser
        const float lo[3] = {lot.x0, 0.0f, lot.z0}, hi[3] = {lot.x1, height(t), lot.z1};
        mesh.addBox(lo, hi, side, scale(side, 1.15f), t);
    }
}

void CompileCity::sharedLot(int file, float& x0, float& z0) const {
    const auto it = std::find(sharedFiles_.begin(), sharedFiles_.end(), file);
    const int i = static_cast<int>(it - sharedFiles_.begin());
    const CityStyle& st = style_;
    x0 = shared_.x0 + st.pad + static_cast<float>(i % shared_.cols) * (st.footprint + st.gap);
    z0 = shared_.z0 + st.pad + static_cast<float>(i / shared_.cols) * (st.footprint + st.gap);
}

void CompileCity::skeletonLot(int index, float& x0, float& z0) const {
    x0 = skeletons_.x0 + kShelfPad + static_cast<float>(index % skeletons_.cols) * (kBook + kBookGap);
    z0 = skeletons_.z0 + kShelfPad + static_cast<float>(index / skeletons_.cols) * (kBook + kBookGap);
}

float CompileCity::skeletonHeight(int index) const {
    const double kb = static_cast<double>(trace().skeletons[static_cast<size_t>(index)].bytes) / 1024.0;
    return 0.2f + 0.25f * static_cast<float>(std::log2(1.0 + kb));
}

Vec3 CompileCity::skeletonTop(int index) const {
    float x0, z0;
    skeletonLot(index, x0, z0);
    return {x0 + kBook / 2, skeletonHeight(index), z0 + kBook / 2};
}

Vec3 CompileCity::sharedTop(int file) const {
    float x0, z0;
    sharedLot(file, x0, z0);
    return {x0 + 0.5f, std::max(0.08f, static_cast<float>(trace().files[static_cast<size_t>(file)].lines) * lineScale_),
            z0 + 0.5f};
}

void CompileCity::buildLibrary(MeshBuilder& mesh) const {
    const CompileTrace& t = trace();
    const int fns = static_cast<int>(functions_.size());
    if (!sharedFiles_.empty()) {
        mesh.addTile(shared_.x0, shared_.z0, shared_.x1, shared_.z1, 0.0f, {0.13f, 0.12f, 0.2f}, -1);
        const int done = replay_.filesDone();
        for (int f : sharedFiles_) {
            if (f >= done) continue;
            const CompileFile& file = t.files[static_cast<size_t>(f)];
            std::vector<int> lines(static_cast<size_t>(fns) + 1, 0);
            for (const auto& kv : file.byFn) {
                const int i = functionIndex(kv.first);
                if (i >= 0) lines[static_cast<size_t>(i)] += kv.second;
            }
            lines.back() += file.unannotated;
            float x0, z0;
            sharedLot(f, x0, z0);
            addStack(mesh, x0, z0, x0 + 1, z0 + 1, lines, sharedTag, f);
        }
    }
    if (!t.skeletons.empty()) {
        mesh.addTile(skeletons_.x0, skeletons_.z0, skeletons_.x1, skeletons_.z1, 0.0f, {0.17f, 0.14f, 0.1f}, -1);
        const int done = replay_.skeletonsDone();
        for (int i = 0; i < done; ++i) {
            float x0, z0;
            skeletonLot(i, x0, z0);
            const std::string& p = t.skeletons[static_cast<size_t>(i)].path;
            // Copied, not generated: one warm colour, headers a shade lighter.
            const bool header = p.size() > 2 && p.compare(p.size() - 2, 2, ".h") == 0;
            const Rgb c = header ? Rgb{0.86f, 0.74f, 0.55f} : Rgb{0.74f, 0.6f, 0.42f};
            const float lo[3] = {x0, 0.0f, z0}, hi[3] = {x0 + kBook, skeletonHeight(i), z0 + kBook};
            mesh.addBox(lo, hi, c, scale(c, 1.15f), skeletonTag(i));
        }
    }
}

void CompileCity::buildMarks(const CompilePick& sel, MeshBuilder& m) const {
    // Marks carry the tag of what they mark, so a click on a pulse still picks its building.
    int32_t tag = -1;
    auto cap = [&](float x0, float z0, float x1, float z1, float y, float h, float grow, Rgb c) {
        const float lo[3] = {x0 - grow, y, z0 - grow}, hi[3] = {x1 + grow, y + h, z1 + grow};
        m.addBox(lo, hi, c, c, tag);
    };
    // Fixer pulses: a cap over each building a recent event visited, in its pass family's colour.
    for (const Replay::Pulse& p : replay_.pulses(replay_.pulseWindow())) {
        const Lot& l = city_.lot(p.type);
        const float h = height(p.type);
        tag = p.type;
        cap(l.x0, l.z0, l.x1, l.z1, h + 0.04f, 0.12f + 0.5f * p.heat, 0.08f + 0.1f * p.heat,
            scale(familyColor(p.family), 0.35f + 0.65f * p.heat));
    }
    // Diff marks: a roof tile on each type the fixer changed (once its fixing is done).
    for (const SchemaType& ty : schema().types) {
        if (!replay_.diffShown(ty.id)) continue;
        const Lot& l = city_.lot(ty.id);
        const float h = height(ty.id);
        tag = ty.id;
        cap(l.x0 + 0.22f, l.z0 + 0.22f, l.x1 - 0.22f, l.z1 - 0.22f, h, 0.14f, 0, {1.0f, 0.3f, 0.6f});
    }
    // The selection: a gold frame above it (a band: around the band).
    const Rgb gold{1, 0.82f, 0.25f};
    tag = sel.kind == CompilePick::Band       ? bandTag(sel.type, sel.fn)
          : sel.kind == CompilePick::Type     ? sel.type
          : sel.kind == CompilePick::Skeleton ? skeletonTag(sel.skeleton)
          : sel.kind == CompilePick::SharedBand ? sharedTag(sel.file, sel.fn)
                                               : -1;
    if ((sel.kind == CompilePick::Type || sel.kind == CompilePick::Band) && sel.type >= 0) {
        const Lot& l = city_.lot(sel.type);
        const float h = height(sel.type);
        if (sel.kind == CompilePick::Band) {
            const std::vector<int> lines = emitted(sel.type);
            const int fns = static_cast<int>(functions_.size());
            float y = 0;
            for (int k = -1; k < fns; ++k) {
                const int fn = k < 0 ? fns : k;
                const int n = lines[static_cast<size_t>(fn)];
                if (n <= 0) continue;
                const float bh = std::max(0.02f, static_cast<float>(n) * lineScale_);
                if (fn == sel.fn) {
                    // Two thin rings, under and over the band.
                    cap(l.x0, l.z0, l.x1, l.z1, y - 0.03f, 0.06f, 0.12f, gold);
                    cap(l.x0, l.z0, l.x1, l.z1, y + bh - 0.03f, 0.06f, 0.12f, gold);
                }
                y += bh;
            }
        }
        cap(l.x0, l.z0, l.x1, l.z1, h + 0.75f, 0.16f, 0.1f, gold);
    } else if (sel.kind == CompilePick::Skeleton && sel.skeleton >= 0) {
        float x0, z0;
        skeletonLot(sel.skeleton, x0, z0);
        cap(x0, z0, x0 + kBook, z0 + kBook, skeletonHeight(sel.skeleton) + 0.5f, 0.12f, 0.08f, gold);
    } else if (sel.kind == CompilePick::SharedBand && sel.file >= 0) {
        const Vec3 top = sharedTop(sel.file);
        cap(top.x - 0.5f, top.z - 0.5f, top.x + 0.5f, top.z + 0.5f, top.y + 0.5f, 0.16f, 0.1f, gold);
    }
}

float CompileCity::tallest() const { return std::max(city_.tallest(), tallestStack_); }

OrbitCamera CompileCity::homeCamera(float aspect) const {
    float x0 = 1e9f, z0 = 1e9f, x1 = -1e9f, z1 = -1e9f;
    auto grow = [&](float a, float b, float c, float d) {
        x0 = std::min(x0, a), z0 = std::min(z0, b), x1 = std::max(x1, c), z1 = std::max(z1, d);
    };
    for (const District& d : city_.districts()) grow(d.x0, d.z0, d.x1, d.z1);
    if (!sharedFiles_.empty()) grow(shared_.x0, shared_.z0, shared_.x1, shared_.z1);
    if (!trace().skeletons.empty()) grow(skeletons_.x0, skeletons_.z0, skeletons_.x1, skeletons_.z1);
    OrbitCamera c;
    c.target = {(x0 + x1) / 2, 0, (z0 + z1) / 2};
    c.yawDeg = -24;
    c.pitchDeg = 38;
    const float w = x1 - x0, d = z1 - z0;
    const float half = std::max(w / 2 / std::max(aspect, 0.3f), d / 2) * 1.2f + tallest() * 0.6f;
    c.distance = half / std::tan(c.fovYDeg * 3.14159265f / 360) + 6;
    c.farPlane = c.distance * 4 + 500;
    return c;
}

}  // namespace asn1viz
