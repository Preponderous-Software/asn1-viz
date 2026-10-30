// Compare mode's geometry (RFC 0034 G4) over the schema city, without an
// engine. Two traces of one schema share its districts and lots; each
// building is a stack of floors, one per emitting function (unannotated
// lines at the bottom, as in the replay), each floor as tall as the larger
// of its A and B line counts:
//
// - the lines both runs wrote, in a muted tint of the function's colour
//   (grey when the type did not change at all);
// - above them, the difference: lines only B wrote (growth) or only A wrote
//   (shrinkage), in a diverging blue / orange pair that does not rely on
//   red against green.
//
// A changed type gets a roof in the colour of its net change (pale when
// lines only moved between functions). A pink roof tile marks a type the fixer treated differently (its printed
// diff, or the number of fixer events naming it). Tags are the replay's
// (bandTag / decodeTag), with the function indexing CompileCompare::functions().
#pragma once

#include <algorithm>

#include "model/compare.h"
#include "scene/city.h"
#include "scene/compile_city.h"

namespace asn1viz {

Rgb growthColor();      // B wrote more
Rgb shrinkColor();      // B wrote fewer
Rgb sharedColor(int fn, int functionCount, bool changedType);
Rgb fixerDiffColor();   // the roof tile
Rgb netColor(int delta); // a changed type's roof: growth, shrinkage, or pale for lines that only moved

class CompareCity {
public:
    CompareCity(const Schema& schema, const CompileCompare& compare, CityStyle style);

    const CityScene& city() const { return city_; }
    const Schema& schema() const { return city_.schema(); }
    const CompileCompare& compare() const { return compare_; }

    // A building's height (its roof): the sum over functions of max(A, B).
    float height(int type) const;
    float lineScale() const { return lineScale_; }
    float tallest() const { return std::max(city_.tallest(), tallestStack_); }

    void buildDistrict(size_t d, MeshBuilder& mesh) const;
    // Fixer marks and the selection's frame.
    void buildMarks(const CompilePick& selected, MeshBuilder& mesh) const;
    OrbitCamera homeCamera(float aspect) const;

private:
    CityScene city_;
    const CompileCompare& compare_;
    float lineScale_ = 0.01f;
    float tallestStack_ = 0;
};

}  // namespace asn1viz
