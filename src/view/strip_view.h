// The bit-strip view on Strata (RFC 0023 section 4): one mesh per lane,
// ray picking to a segment, the pick panel, frame labels projected to the
// screen, byte boundaries and cross-lane links as unlit lines.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "scene/strip.h"
#include "view/shell.h"

namespace asn1viz {

class StripView : public Scene {
public:
    StripView(const Layout& layout, StripStyle style);

    void upload(strata::Engine& engine) override;
    OrbitCamera home(float aspect) const override;
    void pick(const StrataRayHit& hit) override;
    void draw(Overlay& overlay, const CameraMatrices& camera, int width, int height) override;
    bool key(int32_t scancode) override;
    bool focus(Vec3& target) const override;
    void sync(strata::Engine& engine) override;

    // Selects a frame by its path in a lane (headless shots); false if absent.
    bool selectPath(const std::string& path, size_t lane = 0);
    // Draws every frame of one tree depth linked across lanes (0: off).
    void setLinkDepth(int depth) { linkDepth_ = depth; }
    // Leaves out the legend and the key hints (for small renders).
    void setCompact(bool compact) { compact_ = compact; }
    // "file:line" per ASN.1 type name, for the pick panel (from a schema).
    void setTypeLocations(std::map<std::string, std::string> where) { where_ = std::move(where); }

    const StripScene& scene() const { return strips_; }
    bool hasSelection() const { return selLane_ >= 0; }
    std::string selectionPath() const;

private:
    void select(int lane, int segment, int frame, bool wholeFrame);
    void outline(Overlay& o, size_t lane, int64_t start, int64_t len, float height, const float rgba[4]) const;

    StripScene strips_;
    std::vector<uint32_t> meshIds_;
    uint32_t glowId_ = 0;
    bool glowDirty_ = false;
    std::vector<StrataLineVertex> byteTicks_;
    int selLane_ = -1, selSegment_ = -1, selFrame_ = -1;
    bool wholeFrame_ = false;
    int linkDepth_ = 0;
    bool compact_ = false;
    std::map<std::string, std::string> where_;
};

}  // namespace asn1viz
