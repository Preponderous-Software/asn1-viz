// The compilation view on Strata (RFC 0034 section 3): one asn1c run
// replayed over the schema city. Buildings rise in parse order, pulse as
// fixer passes visit them (coloured by pass family) and get a roof mark where
// the fixer changed them, then grow floors of generated C, one band per
// emitting function, and the copied skeletons fill a library at the city's
// edge. A scrubber (drawn on the canvas, drag or click it) spans the run.
//
// Keys: Space play/pause, [ ] a stage, , . a step, L lookup events on/off,
// F focus, H home (the shell). C is kept for compare mode (G4, phase K2).
#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "scene/compile_city.h"
#include "view/shell.h"

namespace asn1viz {

class CompileView : public Scene {
public:
    CompileView(const Schema& schema, const CompileTrace& trace, CityStyle style);

    void upload(strata::Engine& engine) override;
    OrbitCamera home(float aspect) const override;
    void pick(const StrataRayHit& hit) override;
    void draw(Overlay& overlay, const CameraMatrices& camera, int width, int height) override;
    void sync(strata::Engine& engine) override;
    std::vector<PanelLine> panel(size_t fit) const override;
    bool key(int32_t scancode) override;
    bool focus(Vec3& target) const override;
    bool press(float px, float py, bool dragging) override;

    bool selectName(const std::string& name);  // a type, or "Type/function" for a band
    void select(const CompilePick& p);
    void setCompact(bool compact) { compact_ = compact; }
    // The wall clock drives playback; headless renders turn it off and set the playhead.
    void setClock(bool on) { clock_ = on; }

    const CompileCity& scene() const { return city_; }
    Replay& replay() { return city_.replay(); }
    const Replay& replay() const { return city_.replay(); }
    const CompilePick& selected() const { return selected_; }
    std::string selectionName() const;
    // Seeks to a point of the scrubber (0..1 along the bar, as drawn).
    void seekBar(float f) { replay().seek(positionAt(f)); }
    // The playhead as the page shows it: {"stage":..,"pos":..,"steps":..,"fraction":..,"playing":..,"lookups":..}.
    std::string statusJson() const;

    // Compare mode (RFC 0034 G4) is phase K2. This is its seam: a second
    // trace of the same schema will be held here and drawn as floor deltas.
    bool compareAvailable() const { return false; }

private:
    void rebuild(strata::Engine& engine, bool geometry);
    // The scrubber: [x0, x1] on row y; a bar fraction to and from the playhead.
    float barFraction(float position) const;
    float positionAt(float barFraction) const;

    CompileCity city_;
    std::vector<uint32_t> districtIds_;
    uint32_t libraryId_ = 0, marksId_ = 0;
    long long builtKey_ = -1;
    bool marksDirty_ = true;
    CompilePick selected_;
    bool compact_ = false;
    bool clock_ = true;
    std::chrono::steady_clock::time_point last_{};
    bool haveLast_ = false;
    float barX0_ = 0, barX1_ = 0, barY0_ = 0, barY1_ = 0;  // the scrubber's hit box, drawable pixels
    std::string note_;                                    // a short-lived message (compare mode)
    std::chrono::steady_clock::time_point noteUntil_{};
};

}  // namespace asn1viz
