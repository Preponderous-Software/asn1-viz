// Compare mode on Strata (RFC 0034 G4): two asn1-compile/1 traces of one
// schema drawn as one city. Each building's floors show, per emitting
// function, the lines both runs wrote (muted) and the difference: growth in
// blue, shrinkage in orange. Unchanged types are grey; a pink roof tile marks
// a type the fixer treated differently. Picking a floor shows the function,
// its A and B lines and the delta; the panel sums the comparison up.
//
// CompilePair holds a pair of traces as one scene: the replay of A
// (CompileView) and the comparison, with C switching between them.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "model/compare.h"
#include "scene/compare_city.h"
#include "view/compile_view.h"
#include "view/shell.h"

namespace asn1viz {

class CompareView : public Scene {
public:
    CompareView(const Schema& schema, CompileCompare compare, CityStyle style);

    void upload(strata::Engine& engine) override;
    OrbitCamera home(float aspect) const override;
    void pick(const StrataRayHit& hit) override;
    void draw(Overlay& overlay, const CameraMatrices& camera, int width, int height) override;
    void sync(strata::Engine& engine) override;
    std::vector<PanelLine> panel(size_t fit) const override;
    bool key(int32_t scancode) override;
    bool focus(Vec3& target) const override;

    bool selectName(const std::string& name);  // a type, or "Type/function" for a floor
    void select(const CompilePick& p);
    const CompilePick& selected() const { return selected_; }
    std::string selectionName() const;
    void setCompact(bool compact) { compact_ = compact; }

    const CompileCompare& compare() const { return compare_; }
    const CompareCity& scene() const { return city_; }

private:
    void rebuildMarks(strata::Engine& engine);

    CompileCompare compare_;  // before city_, which refers to it
    CompareCity city_;
    std::vector<uint32_t> districtIds_;
    uint32_t marksId_ = 0;
    bool marksDirty_ = true;
    CompilePick selected_;
    bool compact_ = false;
};

// A pair of traces: A's replay, and the comparison of A with B. C switches
// between them, keeping the selected type (and floor, by function name).
class CompilePair : public Scene {
public:
    CompilePair(std::unique_ptr<CompileView> replay, std::unique_ptr<CompareView> compare, bool comparing);

    void upload(strata::Engine& engine) override;
    OrbitCamera home(float aspect) const override { return active().home(aspect); }
    void pick(const StrataRayHit& hit) override { active().pick(hit); }
    void draw(Overlay& overlay, const CameraMatrices& camera, int width, int height) override;
    void sync(strata::Engine& engine) override;
    std::vector<PanelLine> panel(size_t fit) const override { return active().panel(fit); }
    bool key(int32_t scancode) override;
    bool focus(Vec3& target) const override { return active().focus(target); }
    bool press(float px, float py, bool dragging) override { return active().press(px, py, dragging); }

    bool comparing() const { return comparing_; }
    void setComparing(bool on);
    CompileView& replay() { return *replay_; }
    const CompileView& replay() const { return *replay_; }
    CompareView& comparison() { return *compare_; }
    const CompareView& comparison() const { return *compare_; }
    std::string selectionName() const;
    bool selectName(const std::string& name);

private:
    Scene& active() { return comparing_ ? static_cast<Scene&>(*compare_) : *replay_; }
    const Scene& active() const { return comparing_ ? static_cast<const Scene&>(*compare_) : *replay_; }

    std::unique_ptr<CompileView> replay_;
    std::unique_ptr<CompareView> compare_;
    bool comparing_ = false;
    bool swapped_ = false;  // the other view's meshes are up: replace them in sync
};

// The replay a scene shows now (a CompileView, or a pair not comparing), or null.
CompileView* replayOf(Scene& scene);
const CompileView* replayOf(const Scene& scene);

}  // namespace asn1viz
