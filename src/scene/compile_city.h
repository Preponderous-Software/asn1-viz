// The compilation view's geometry (RFC 0034 section 3) over the schema city,
// without an engine. The city's districts and lots are reused unchanged;
// what changes with the replay is drawn by rebuilding the few meshes that
// carry it, never by adding a mesh per building:
//
// - one mesh per district: its ground and its buildings at their current
//   height (rising in Parse; in Compile and after, a stack of floors, one
//   band per emitting function, its height following its line count);
// - one unlit mesh of marks: fixer pulses, diff marks on roofs, the selection;
// - one "library" mesh at the city's edge: the generated files no type owns,
//   and the skeletons copied in the Save stage.
//
// Every triangle is tagged, so a pick names a type, a band (type and
// function), a shared file's band or a skeleton.
#pragma once

#include <string>
#include <vector>

#include "model/compile.h"
#include "model/schema.h"
#include "scene/city.h"
#include "scene/replay.h"

namespace asn1viz {

// What a tag names.
struct CompilePick {
    enum Kind { None, Type, Band, SharedBand, Skeleton } kind = None;
    int type = -1;   // Type, Band
    int fn = -1;     // Band, SharedBand: an index into CompileCity::functions(), or its size for unannotated lines
    int file = -1;   // SharedBand: an index into trace.files
    int skeleton = -1;
};

int32_t bandTag(int type, int fn);
int32_t sharedTag(int file, int fn);
int32_t skeletonTag(int index);
CompilePick decodeTag(int32_t tag);

Rgb functionColor(int fn, int functionCount);  // fn == functionCount: unannotated
Rgb familyColor(PassFamily f);

class CompileCity {
public:
    CompileCity(const Schema& schema, const CompileTrace& trace, CityStyle style);

    const CityScene& city() const { return city_; }
    const Schema& schema() const { return city_.schema(); }
    const CompileTrace& trace() const { return replay_.trace(); }
    Replay& replay() { return replay_; }
    const Replay& replay() const { return replay_; }
    // The emitting functions: the trace's list, plus any a file names that
    // the list leaves out; a band's fn indexes this.
    const std::vector<std::string>& functions() const { return functions_; }
    int functionIndex(const std::string& fn) const;  // -1 if none

    // Lines per function (and unannotated, at index functions.size()) a
    // type's files have emitted so far, and their total.
    std::vector<int> emitted(int type) const;
    int emittedTotal(int type) const;
    // A building's height now (its roof).
    float height(int type) const;
    // World units per generated line.
    float lineScale() const { return lineScale_; }

    // A key that changes whenever the district or library meshes must be rebuilt.
    long long geometryKey() const;
    void buildDistrict(size_t d, MeshBuilder& mesh) const;
    void buildLibrary(MeshBuilder& mesh) const;
    // Pulses, diff marks and the selection's cap.
    void buildMarks(const CompilePick& selected, MeshBuilder& mesh) const;

    // The library's two shelves (shared files, then skeletons), on the ground.
    struct Shelf {
        float x0, z0, x1, z1;
        int cols;
    };
    const Shelf& sharedShelf() const { return shared_; }
    const Shelf& skeletonShelf() const { return skeletons_; }
    Vec3 skeletonTop(int index) const;
    Vec3 sharedTop(int file) const;
    // Where a skeleton or a shared file stands (its lot's corner).
    void skeletonLot(int index, float& x0, float& z0) const;
    void sharedLot(int file, float& x0, float& z0) const;
    float skeletonHeight(int index) const;

    OrbitCamera homeCamera(float aspect) const;
    float tallest() const;  // the tallest a building or shelf gets in the run

    std::vector<int> sharedFiles() const { return sharedFiles_; }

private:
    void addStack(MeshBuilder& mesh, float x0, float z0, float x1, float z1, const std::vector<int>& lines,
                  int32_t (*tagOf)(int, int), int owner) const;

    CityScene city_;
    Replay replay_;
    CityStyle style_;
    std::vector<std::string> functions_;
    std::vector<std::vector<int>> filesOf_;  // type -> file indexes, in trace order
    std::vector<int> sharedFiles_;           // files with no type
    float lineScale_ = 0.01f;
    float tallestStack_ = 0;
    Shelf shared_{}, skeletons_{};
};

}  // namespace asn1viz
