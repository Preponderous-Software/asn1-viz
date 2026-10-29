// asn1-schema/1 (RFC 0024 section 1): a parsed, fixed ASN.1 specification as
// modules, top-level assignments, the references between them and the
// recursive groups. Read-only model; no engine.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "model/json.h"

namespace asn1viz {

struct SchemaModule {
    std::string name, file;
    std::vector<std::string> imports;
};

struct SchemaType {
    int id = 0, module = 0;
    std::string name, kind;
    int members = 0, line = 0;
    int scc = -1;  // index into Schema::sccs, or -1
    // Bytes of the C asn1c generates for it (RFC 0034: written by
    // asn1-viz-compile-trace), or -1 where the file says null.
    int64_t codeBytes = -1;

    // A value, value set, object or field: a flat tile, not a building.
    bool flat() const;
};

struct SchemaRef {
    int from = 0, to = 0;
    std::string member;  // the member the reference sits in, if any
};

struct Schema {
    std::string asn1cCommit;
    std::vector<SchemaModule> modules;
    std::vector<SchemaType> types;
    std::vector<SchemaRef> refs;
    std::vector<std::vector<int>> sccs;
    // Derived: reference indexes out of and into each type.
    std::vector<std::vector<int>> out, in;

    int typeByName(const std::string& name) const;  // the first with that name, or -1
    int moduleByName(const std::string& name) const;
    // "file:line" per type name (the first definition), for the strip view's pick panel.
    std::map<std::string, std::string> locations() const;
    // Types reachable from `from` by following references, up to `hops`;
    // each with the hop it was first reached at.
    std::map<int, int> reach(int from, int hops) const;
};

Schema loadSchema(const json::Value& doc);
Schema loadSchemaFile(const std::string& path);

}  // namespace asn1viz
