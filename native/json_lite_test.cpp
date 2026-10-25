#include "json_lite.hpp"
#include <cstdio>

static int failures = 0;
static void Check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++failures;
}

int main() {
    using hp_json::Value;
    Value v;
    Check(hp_json::Parse(R"({"player":"Ann","relayPort":5191,"standIns":true,"mine":[],)"
        R"("games":[{"id":"6f1c","name":"Duel","maxPlayers":4,"password":false}],)"
        R"("chat":[{"id":7,"text":"a\"b\\c\u00e9\ud83d\ude00\n"}]})", v), "parses a lobby reply");
    Check(v["player"].str() == "Ann", "reads a string member");
    Check(v["relayPort"].integer() == 5191, "reads an integer member");
    Check(v["standIns"].truthy(), "reads a boolean member");
    Check(v["mine"].size() == 0 && v["games"].size() == 1, "reads array sizes");
    Check(v["games"][0]["name"].str() == "Duel" && v["games"][0]["maxPlayers"].integer() == 4, "reads nested members");
    Check(v["chat"][0]["text"].str() == "a\"b\\c\xC3\xA9\xF0\x9F\x98\x80\n", "decodes escapes and surrogate pairs to UTF-8");
    Check(v["missing"]["deeper"].str().empty() && v["games"][5]["name"].integer() == 0, "missing members read as empty");
    Check(hp_json::Parse("  [1, -2.5e2, null, false]  ", v) && v[1].number == -250 && v[2].kind == Value::Null, "parses scalars");
    Check(!hp_json::Parse("{\"a\":1,}", v), "rejects a trailing comma");
    Check(!hp_json::Parse("{\"a\":1} x", v), "rejects trailing content");
    Check(!hp_json::Parse("\"\\ud800\"", v), "rejects a lone surrogate");
    Check(!hp_json::Parse(std::string(100, '[') + std::string(100, ']'), v), "limits nesting");
    Check(!hp_json::Parse("\"unterminated", v), "rejects an unterminated string");
    std::printf(failures ? "FAILED\n" : "ALL PASSED\n");
    return failures ? 1 : 0;
}
