#include "json_util.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ptstt {

std::string json_quote(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    o += buf;
                } else {
                    o += static_cast<char>(c);
                }
        }
    }
    return o + "\"";
}

namespace {

// A small cursor over the JSON text. Only what the VAD documents need:
// objects, arrays, numbers, strings without escapes, true/false/null.
struct Cursor {
    const char* p;

    void ws() {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') ++p;
    }
    bool eat(char c) {
        ws();
        if (*p != c) return false;
        ++p;
        return true;
    }
    bool string(std::string& out) {
        ws();
        if (*p != '"') return false;
        const char* s = ++p;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) ++p;
            ++p;
        }
        if (*p != '"') return false;
        out.assign(s, p);
        ++p;
        return true;
    }
    bool number(double& out) {
        ws();
        char* end = nullptr;
        out = std::strtod(p, &end);
        if (end == p) return false;
        p = end;
        return true;
    }
    // Skips any value.
    bool skip() {
        ws();
        if (*p == '"') {
            std::string s;
            return string(s);
        }
        if (*p == '{' || *p == '[') {
            const char open = *p, close = open == '{' ? '}' : ']';
            ++p;
            if (eat(close)) return true;
            for (;;) {
                if (open == '{') {
                    std::string k;
                    if (!string(k) || !eat(':')) return false;
                }
                if (!skip()) return false;
                if (eat(close)) return true;
                if (!eat(',')) return false;
            }
        }
        if (!std::strncmp(p, "true", 4)) { p += 4; return true; }
        if (!std::strncmp(p, "false", 5)) { p += 5; return true; }
        if (!std::strncmp(p, "null", 4)) { p += 4; return true; }
        double d;
        return number(d);
    }
};

bool parse_event(Cursor& c, VadEvent& ev) {
    if (!c.eat('{')) return false;
    std::string type;
    double time = 0;
    if (c.eat('}')) return false;
    for (;;) {
        std::string key;
        if (!c.string(key) || !c.eat(':')) return false;
        if (key == "type") {
            if (!c.string(type)) return false;
        } else if (key == "time") {
            if (!c.number(time)) return false;
        } else if (!c.skip()) {
            return false;
        }
        if (c.eat('}')) break;
        if (!c.eat(',')) return false;
    }
    ev.start = type == "start";
    ev.time = time;
    return type == "start" || type == "end";
}

}  // namespace

bool parse_vad_feed(const char* json, VadFeed& out) {
    out = VadFeed{};
    if (!json) return false;
    Cursor c{json};
    if (!c.eat('{')) return false;
    if (c.eat('}')) return true;
    for (;;) {
        std::string key;
        if (!c.string(key) || !c.eat(':')) return false;
        if (key == "frame_sec") {
            if (!c.number(out.frame_sec)) return false;
        } else if (key == "first_frame") {
            double d;
            if (!c.number(d)) return false;
            out.first_frame = static_cast<long>(d);
        } else if (key == "events") {
            if (!c.eat('[')) return false;
            if (!c.eat(']')) {
                for (;;) {
                    VadEvent ev{};
                    if (!parse_event(c, ev)) return false;
                    out.events.push_back(ev);
                    if (c.eat(']')) break;
                    if (!c.eat(',')) return false;
                }
            }
        } else if (key == "probabilities") {
            if (!c.eat('[')) return false;
            if (!c.eat(']')) {
                for (;;) {
                    double d;
                    if (!c.number(d)) return false;
                    out.probabilities.push_back(static_cast<float>(d));
                    if (c.eat(']')) break;
                    if (!c.eat(',')) return false;
                }
            }
        } else if (!c.skip()) {
            return false;
        }
        if (c.eat('}')) return true;
        if (!c.eat(',')) return false;
    }
}

}  // namespace ptstt
