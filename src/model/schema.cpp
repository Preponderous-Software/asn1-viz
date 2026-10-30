#include "model/schema.h"

#include <deque>
#include <stdexcept>

namespace asn1viz {

bool SchemaType::flat() const {
    return kind == "value" || kind == "value set" || kind == "object" || kind == "object field";
}

int Schema::typeByName(const std::string& name) const {
    for (const SchemaType& t : types)
        if (t.name == name) return t.id;
    return -1;
}

int Schema::moduleByName(const std::string& name) const {
    for (size_t i = 0; i < modules.size(); ++i)
        if (modules[i].name == name) return static_cast<int>(i);
    return -1;
}

std::map<std::string, std::string> Schema::locations() const {
    std::map<std::string, std::string> where;
    for (const SchemaType& t : types)
        if (!t.flat() && !where.count(t.name))
            where[t.name] = modules[static_cast<size_t>(t.module)].file + ":" + std::to_string(t.line);
    return where;
}

std::map<int, int> Schema::reach(int from, int hops) const {
    std::map<int, int> seen;
    if (from < 0 || from >= static_cast<int>(types.size())) return seen;
    std::deque<int> q{from};
    seen[from] = 0;
    while (!q.empty()) {
        const int t = q.front();
        q.pop_front();
        if (seen[t] >= hops) continue;
        for (int r : out[static_cast<size_t>(t)]) {
            const int to = refs[static_cast<size_t>(r)].to;
            if (seen.count(to)) continue;
            seen[to] = seen[t] + 1;
            q.push_back(to);
        }
    }
    return seen;
}

Schema loadSchema(const json::Value& doc) {
    if (doc.str("format") != "asn1-schema/1") throw std::runtime_error("not an asn1-schema/1 document");
    Schema s;
    s.asn1cCommit = doc.get("asn1c").str("commit");
    for (const json::Value& m : doc.at("modules").items()) {
        SchemaModule mod;
        mod.name = m.str("name");
        mod.file = m.str("file");
        if (m.get("imports").isArray())
            for (const json::Value& i : m.at("imports").items()) mod.imports.push_back(i.asString());
        s.modules.push_back(std::move(mod));
    }
    for (const json::Value& t : doc.at("types").items()) {
        SchemaType ty;
        ty.id = static_cast<int>(t.integer("id"));
        ty.module = static_cast<int>(t.integer("module"));
        ty.name = t.str("name");
        ty.kind = t.str("kind");
        ty.members = static_cast<int>(t.integer("members"));
        ty.line = static_cast<int>(t.integer("line"));
        ty.scc = static_cast<int>(t.integer("scc", -1));
        ty.codeBytes = t.integer("codeBytes", -1);
        if (ty.id != static_cast<int>(s.types.size())) throw std::runtime_error("asn1-schema/1: type ids out of order");
        if (ty.module < 0 || ty.module >= static_cast<int>(s.modules.size()))
            throw std::runtime_error("asn1-schema/1: a type names no module");
        s.types.push_back(std::move(ty));
    }
    const int n = static_cast<int>(s.types.size());
    s.out.resize(s.types.size());
    s.in.resize(s.types.size());
    for (const json::Value& r : doc.at("refs").items()) {
        SchemaRef ref;
        ref.from = static_cast<int>(r.integer("from"));
        ref.to = static_cast<int>(r.integer("to"));
        ref.member = r.str("member");
        if (ref.from < 0 || ref.from >= n || ref.to < 0 || ref.to >= n)
            throw std::runtime_error("asn1-schema/1: a reference names no type");
        s.out[static_cast<size_t>(ref.from)].push_back(static_cast<int>(s.refs.size()));
        s.in[static_cast<size_t>(ref.to)].push_back(static_cast<int>(s.refs.size()));
        s.refs.push_back(std::move(ref));
    }
    if (doc.get("sccs").isArray())
        for (const json::Value& c : doc.at("sccs").items()) {
            std::vector<int> group;
            for (const json::Value& t : c.items()) group.push_back(static_cast<int>(t.asInt()));
            s.sccs.push_back(std::move(group));
        }
    return s;
}

Schema loadSchemaFile(const std::string& path) { return loadSchema(json::parseFile(path)); }

}  // namespace asn1viz
