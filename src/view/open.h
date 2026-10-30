// Opens an asn1-layout/1, asn1-schema/1 or asn1-compile/1 file as the view
// that draws it (the bit strips, the schema city or the compilation replay). Shared by the desktop and browser
// viewers.
#pragma once

#include <memory>
#include <string>

#include "view/shell.h"

namespace asn1viz {

struct OpenOptions {
    std::string pick;    // strips: a field path; city: a type name
    int lane = 0;        // strips: the lane `pick` is in
    int links = 0;       // strips: link every field of this tree depth across lanes
    int rowBits = 0;     // strips: bits per row (0: by the encoding's size)
    std::string schema;  // strips: an asn1-schema/1 file for file:line (default: schema.json beside it)
    int hops = 1;        // city: how far the selection's references are followed
    bool loops = true;   // city: draw recursion loops
    bool compact = false; // leave out the legend and key hints
    // compile: the playhead as a fraction of the run (< 0: from the start,
    // playing), and the asn1-schema/1 file (default: the trace's "schema",
    // beside it).
    float at = -1;
    bool hideLookups = false;
};

std::unique_ptr<Scene> openScene(const std::string& file, const OpenOptions& options);

// What is selected, as a path (strips), a type name (city) or a type,
// "Type/function" band or skeleton (compile); "" for nothing.
std::string selectionOf(const Scene& scene);

}  // namespace asn1viz
