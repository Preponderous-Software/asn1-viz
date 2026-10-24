// The schema city on Strata (RFC 0024 sections 3 and 4): one mesh per
// district, ray picking to a type, IMPORTS arcs always on, the selected
// building's references (outbound and inbound) as unlit arcs, a hop depth
// for "what does this pull in", recursion loops, and screen-space labels.
#pragma once

#include <string>
#include <vector>

#include "scene/city.h"
#include "view/shell.h"

namespace asn1viz {

class CityView : public Scene {
public:
    CityView(const Schema& schema, CityStyle style);

    void upload(strata::Engine& engine) override;
    OrbitCamera home(float aspect) const override;
    void pick(const StrataRayHit& hit) override;
    void draw(Overlay& overlay, const CameraMatrices& camera, int width, int height) override;
    void sync(strata::Engine& engine) override;
    bool key(int32_t scancode) override;
    bool focus(Vec3& target) const override;

    bool selectName(const std::string& name);  // false if there is no such type
    void setHops(int hops) { hops_ = std::max(1, std::min(hops, 4)); dirty_ = true; }
    void setShowRecursion(bool on) { recursion_ = on; }
    void setCompact(bool compact) { compact_ = compact; }

    const CityScene& city() const { return city_; }
    int selected() const { return selected_; }

private:
    void select(int type);

    CityScene city_;
    std::vector<uint32_t> districtIds_;
    uint32_t roofId_ = 0, glowId_ = 0;
    bool dirty_ = true;
    int selected_ = -1;
    int hops_ = 1;
    bool recursion_ = true;
    bool compact_ = false;
};

}  // namespace asn1viz
