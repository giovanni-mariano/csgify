#pragma once

// Minimal recursive-descent JSON parser, scoped to what csgify's
// decision-log writer and recipe compactor emit (objects, arrays,
// strings, numbers, bools, nulls). Header-only, no dependency.

#include <cctype>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace csgify {

struct JsonValue;
using JsonObject = std::map<std::string, std::shared_ptr<JsonValue>>;
using JsonArray = std::vector<std::shared_ptr<JsonValue>>;

enum class JsonKind { null, boolean, number, string, array, object };

struct JsonValue {
    JsonKind kind = JsonKind::null;
    bool b = false;
    double num = 0.0;
    std::string str;
    JsonArray arr;
    JsonObject obj;

    const JsonValue* get(const std::string& key) const {
        if (kind != JsonKind::object) return nullptr;
        auto it = obj.find(key);
        if (it == obj.end()) return nullptr;
        return it->second.get();
    }
    std::string as_string(const std::string& fallback = {}) const {
        return kind == JsonKind::string ? str : fallback;
    }
    double as_number(double fallback = 0.0) const {
        return kind == JsonKind::number ? num : fallback;
    }
    bool as_bool(bool fallback = false) const {
        return kind == JsonKind::boolean ? b : fallback;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& src) : src_(src) {}

    std::shared_ptr<JsonValue> parse() {
        skip_ws();
        auto v = parse_value();
        skip_ws();
        if (pos_ != src_.size()) fail("trailing characters");
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& msg) {
        std::ostringstream out;
        out << "json parse error at " << pos_ << ": " << msg;
        throw std::runtime_error(out.str());
    }
    void skip_ws() {
        while (pos_ < src_.size() &&
               (src_[pos_] == ' ' || src_[pos_] == '\t' ||
                src_[pos_] == '\n' || src_[pos_] == '\r')) {
            ++pos_;
        }
    }
    char peek() {
        if (pos_ >= src_.size()) fail("unexpected end of input");
        return src_[pos_];
    }
    void expect(char ch) {
        if (peek() != ch) fail(std::string("expected '") + ch + "'");
        ++pos_;
    }

    static int hex_value(char ch) {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
        if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
        return -1;
    }

    unsigned parse_hex4() {
        if (pos_ + 4 > src_.size()) fail("unterminated \\u escape");
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const int h = hex_value(src_[pos_++]);
            if (h < 0) fail("invalid \\u escape");
            value = (value << 4) | static_cast<unsigned>(h);
        }
        return value;
    }

    void append_utf8(unsigned codepoint, std::string* out) {
        if (codepoint <= 0x7f) {
            out->push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ff) {
            out->push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
            out->push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else if (codepoint <= 0xffff) {
            out->push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
            out->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            out->push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else if (codepoint <= 0x10ffff) {
            out->push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
            out->push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
            out->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            out->push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else {
            fail("unicode codepoint out of range");
        }
    }

    std::shared_ptr<JsonValue> parse_value() {
        skip_ws();
        const char c = peek();
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '"') return parse_string_value();
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        if (c == 't' || c == 'f') return parse_bool();
        if (c == 'n') return parse_null();
        fail("unexpected character");
    }
    std::shared_ptr<JsonValue> parse_object() {
        auto v = std::make_shared<JsonValue>();
        v->kind = JsonKind::object;
        expect('{');
        skip_ws();
        if (peek() == '}') { ++pos_; return v; }
        while (true) {
            skip_ws();
            const std::string key = parse_string_raw();
            skip_ws();
            expect(':');
            v->obj[key] = parse_value();
            skip_ws();
            const char c = peek();
            if (c == ',') { ++pos_; continue; }
            if (c == '}') { ++pos_; return v; }
            fail("expected ',' or '}'");
        }
    }
    std::shared_ptr<JsonValue> parse_array() {
        auto v = std::make_shared<JsonValue>();
        v->kind = JsonKind::array;
        expect('[');
        skip_ws();
        if (peek() == ']') { ++pos_; return v; }
        while (true) {
            v->arr.push_back(parse_value());
            skip_ws();
            const char c = peek();
            if (c == ',') { ++pos_; continue; }
            if (c == ']') { ++pos_; return v; }
            fail("expected ',' or ']'");
        }
    }
    std::string parse_string_raw() {
        expect('"');
        std::string out;
        while (pos_ < src_.size()) {
            const char c = src_[pos_++];
            if (c == '"') return out;
            if (c == '\\') {
                if (pos_ >= src_.size()) fail("unterminated escape");
                const char e = src_[pos_++];
                switch (e) {
                case '"':  out += '"'; break;
                case '\\': out += '\\'; break;
                case '/':  out += '/'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'u': {
                    unsigned codepoint = parse_hex4();
                    if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                        if (pos_ + 2 > src_.size()
                            || src_[pos_] != '\\'
                            || src_[pos_ + 1] != 'u') {
                            fail("missing low surrogate");
                        }
                        pos_ += 2;
                        const unsigned low = parse_hex4();
                        if (low < 0xdc00 || low > 0xdfff) {
                            fail("invalid low surrogate");
                        }
                        codepoint = 0x10000
                            + ((codepoint - 0xd800) << 10)
                            + (low - 0xdc00);
                    } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                        fail("unexpected low surrogate");
                    }
                    append_utf8(codepoint, &out);
                    break;
                }
                default: fail("unknown escape");
                }
            } else {
                if (static_cast<unsigned char>(c) < 0x20) {
                    fail("unescaped control character in string");
                }
                out += c;
            }
        }
        fail("unterminated string");
    }
    std::shared_ptr<JsonValue> parse_string_value() {
        auto v = std::make_shared<JsonValue>();
        v->kind = JsonKind::string;
        v->str = parse_string_raw();
        return v;
    }
    std::shared_ptr<JsonValue> parse_number() {
        const std::size_t start = pos_;
        if (peek() == '-') ++pos_;
        if (pos_ >= src_.size()) fail("invalid number");
        if (src_[pos_] == '0') {
            ++pos_;
            if (pos_ < src_.size()
                && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                fail("invalid leading zero");
            }
        } else if (src_[pos_] >= '1' && src_[pos_] <= '9') {
            while (pos_ < src_.size()
                   && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                ++pos_;
            }
        } else {
            fail("invalid number");
        }
        if (pos_ < src_.size() && src_[pos_] == '.') {
            ++pos_;
            if (pos_ >= src_.size()
                || !std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                fail("invalid fraction");
            }
            while (pos_ < src_.size()
                   && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                ++pos_;
            }
        }
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) {
                ++pos_;
            }
            if (pos_ >= src_.size()
                || !std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                fail("invalid exponent");
            }
            while (pos_ < src_.size()
                   && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                ++pos_;
            }
        }
        auto v = std::make_shared<JsonValue>();
        v->kind = JsonKind::number;
        try {
            std::size_t consumed = 0;
            const std::string token = src_.substr(start, pos_ - start);
            v->num = std::stod(token, &consumed);
            if (consumed != token.size() || !std::isfinite(v->num)) {
                fail("invalid number");
            }
        } catch (const std::exception&) {
            fail("invalid number");
        }
        return v;
    }
    std::shared_ptr<JsonValue> parse_bool() {
        if (src_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            auto v = std::make_shared<JsonValue>();
            v->kind = JsonKind::boolean;
            v->b = true;
            return v;
        }
        if (src_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            auto v = std::make_shared<JsonValue>();
            v->kind = JsonKind::boolean;
            v->b = false;
            return v;
        }
        fail("invalid literal");
    }
    std::shared_ptr<JsonValue> parse_null() {
        if (src_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            return std::make_shared<JsonValue>();
        }
        fail("invalid literal");
    }

    const std::string& src_;
    std::size_t pos_ = 0;
};

inline std::shared_ptr<JsonValue> load_json_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("could not open " + path);
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return JsonParser(buf.str()).parse();
}

} // namespace csgify
