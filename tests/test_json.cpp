#include <cmath>
#include <string>

#include "check.h"
#include "model/json.h"

using namespace asn1viz;

int main() {
    const json::Value v = json::parse(R"( {"format":"asn1-layout/1","n":8248,"neg":-1.5e2,"ok":true,"no":false,
        "nil":null,"list":[1,2,{"a":"b"}],"esc":"q\"\\\/\n\u00e9\ud83d\ude00"} )");
    CHECK(v.isObject());
    CHECK(v.str("format") == "asn1-layout/1");
    CHECK(v.integer("n") == 8248);
    CHECK(v.at("neg").asNumber() == -150.0);
    CHECK(v.at("ok").asBool());
    CHECK(!v.at("no").asBool());
    CHECK(v.at("nil").isNull());
    CHECK(v.at("list").items().size() == 3);
    CHECK(v.at("list").items()[2].str("a") == "b");
    CHECK(v.at("esc").asString() == "q\"\\/\n\xc3\xa9\xf0\x9f\x98\x80");
    CHECK(v.get("missing").isNull());
    CHECK(v.integer("missing", 7) == 7);
    CHECK(!v.has("missing"));

    // Errors carry a byte offset and never crash.
    const char* bad[] = {"", "{", "[1,]", "{\"a\" 1}", "tru", "\"abc", "1 2", "[\"\\x\"]", "-", "\"\\ud800\""};
    for (const char* b : bad) {
        bool threw = false;
        try {
            json::parse(b);
        } catch (const json::ParseError&) {
            threw = true;
        }
        CHECK(threw);
    }
    // Deep nesting is refused, not a stack overflow.
    bool deep = false;
    try {
        json::parse(std::string(100000, '['));
    } catch (const json::ParseError&) {
        deep = true;
    }
    CHECK(deep);
    // Type mismatches throw.
    bool mismatch = false;
    try {
        (void)v.at("n").asString();
    } catch (const std::runtime_error&) {
        mismatch = true;
    }
    CHECK(mismatch);
    TEST_MAIN_END();
}
