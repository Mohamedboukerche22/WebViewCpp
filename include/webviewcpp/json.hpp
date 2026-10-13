// WebViewCpp — minimal, dependency-free JSON with an ordered object model.
//
// The IPC bridge and the asset manifest both need JSON. Rather than pull in
// nlohmann/json (MIT, ~900 KB) we ship a compact implementation. It is not a
// general-purpose library: it favours a small, predictable surface over
// exhaustive spec coverage. Numbers keep their integer/real distinction so the
// JS side never sees "1" become 1.0.
#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace wvc {

class Json {
public:
    using array = std::vector<Json>;
    using object = std::map<std::string, Json>;  // ordered for deterministic dumps

    enum class Type { Null, Bool, Int, Real, String, Array, Object };

    Json() noexcept : v_(std::monostate{}) {}
    Json(std::nullptr_t) noexcept : v_(std::monostate{}) {}
    Json(bool b) noexcept : v_(b) {}
    Json(int i) : v_(static_cast<int64_t>(i)) {}
    Json(int64_t i) noexcept : v_(i) {}
    Json(uint64_t i) : v_(static_cast<int64_t>(i)) {}
    Json(double d) noexcept : v_(d) {}
    Json(const char* s) : v_(std::string(s)) {}
    Json(std::string s) : v_(std::move(s)) {}
    Json(std::string_view s) : v_(std::string(s)) {}
    Json(array a) : v_(std::move(a)) {}
    Json(object o) : v_(std::move(o)) {}

    // Brace construction. A list whose every element is a 2-item array with a
    // string key is interpreted as an object (so `Json{{"a", 1}}` is {"a":1});
    // anything else is an array (so `Json{1, 2, 3}` is [1,2,3]). Use the
    // explicit `array{...}` / `object{{...}}` forms when you need to be sure.
    Json(std::initializer_list<Json> init) {
        const bool looks_like_object = [&] {
            if (init.size() == 0) return false;
            for (const auto& element : init) {
                if (!element.is_array() || element.size() != 2 || !element[0].is_string())
                    return false;
            }
            return true;
        }();
        if (looks_like_object) {
            object o;
            for (const auto& element : init) o[element[0].as_string()] = element[1];
            v_ = std::move(o);
        } else {
            v_ = array(init);
        }
    }

    Type type() const noexcept {
        switch (v_.index()) {
            case 0: return Type::Null;
            case 1: return Type::Bool;
            case 2: return Type::Int;
            case 3: return Type::Real;
            case 4: return Type::String;
            case 5: return Type::Array;
            default: return Type::Object;
        }
    }

    bool is_null()   const noexcept { return type() == Type::Null; }
    bool is_bool()   const noexcept { return type() == Type::Bool; }
    bool is_int()    const noexcept { return type() == Type::Int; }
    bool is_real()   const noexcept { return type() == Type::Real; }
    bool is_number() const noexcept { return is_int() || is_real(); }
    bool is_string() const noexcept { return type() == Type::String; }
    bool is_array()  const noexcept { return type() == Type::Array; }
    bool is_object() const noexcept { return type() == Type::Object; }

    // --- typed accessors (throwing) -------------------------------------
    bool as_bool() const { return std::get<bool>(v_); }
    int64_t as_int() const {
        if (is_int())  return std::get<int64_t>(v_);
        if (is_real()) return static_cast<int64_t>(std::get<double>(v_));
        throw std::runtime_error("json: not a number");
    }
    double as_double() const {
        if (is_real()) return std::get<double>(v_);
        if (is_int())  return static_cast<double>(std::get<int64_t>(v_));
        throw std::runtime_error("json: not a number");
    }
    const std::string& as_string() const { return std::get<std::string>(v_); }
    const array& as_array() const { return std::get<array>(v_); }
    array& as_array() { return std::get<array>(v_); }
    const object& as_object() const { return std::get<object>(v_); }
    object& as_object() { return std::get<object>(v_); }

    // --- safe accessors with defaults -----------------------------------
    bool bool_or(bool d) const { return is_bool() ? as_bool() : d; }
    int64_t int_or(int64_t d) const { return is_number() ? as_int() : d; }
    double double_or(double d) const { return is_number() ? as_double() : d; }
    std::string string_or(std::string d) const { return is_string() ? as_string() : std::move(d); }

    // --- containers ------------------------------------------------------
    std::size_t size() const {
        if (is_array())  return as_array().size();
        if (is_object()) return as_object().size();
        return 0;
    }
    bool empty() const { return size() == 0; }
    bool contains(std::string_view key) const {
        return is_object() && as_object().find(std::string(key)) != as_object().end();
    }

    // Object access. Reading a missing key returns a shared null sentinel,
    // so `j["missing"]["deeper"]` never throws. Writing promotes to object.
    const Json& operator[](std::string_view key) const {
        static const Json kNull;
        if (!is_object()) return kNull;
        auto it = as_object().find(std::string(key));
        return it == as_object().end() ? kNull : it->second;
    }
    Json& operator[](std::string_view key) {
        if (!is_object()) v_ = object{};
        return as_object()[std::string(key)];
    }

    const Json& operator[](std::size_t i) const {
        static const Json kNull;
        if (!is_array() || i >= as_array().size()) return kNull;
        return as_array()[i];
    }
    Json& operator[](std::size_t i) {
        if (!is_array()) v_ = array{};
        auto& a = as_array();
        if (i >= a.size()) a.resize(i + 1);
        return a[i];
    }

    const Json& at(std::string_view key) const {
        if (is_object()) {
            auto it = as_object().find(std::string(key));
            if (it != as_object().end()) return it->second;
        }
        throw std::runtime_error("json: key not found: " + std::string(key));
    }

    void push_back(Json value) {
        if (!is_array()) v_ = array{};
        as_array().push_back(std::move(value));
    }

    // --- serialisation ---------------------------------------------------
    std::string dump(int indent = -1) const {
        std::string out;
        out.reserve(128);
        dump_into(out, indent, 0);
        return out;
    }

    // Parse. On failure throws std::runtime_error with a byte offset.
    static Json parse(std::string_view text) {
        Parser p{text};
        Json j = p.parse_value();
        p.skip_ws();
        if (p.pos != text.size())
            throw std::runtime_error("json: trailing data at offset " + std::to_string(p.pos));
        return j;
    }

    static Json parse_or(std::string_view text, Json fallback) {
        try { return parse(text); } catch (...) { return fallback; }
    }

private:
    std::variant<std::monostate, bool, int64_t, double, std::string, array, object> v_;

    static void escape_into(std::string& out, std::string_view s) {
        out.push_back('"');
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                case '\b': out += "\\b";  break;
                case '\f': out += "\\f";  break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out.push_back(c);
                    }
            }
        }
        out.push_back('"');
    }

    void dump_into(std::string& out, int indent, int depth) const {
        const bool pretty = indent >= 0;
        const std::string pad = pretty ? std::string((depth + 1) * indent, ' ') : std::string{};
        const std::string pad_end = pretty ? std::string(depth * indent, ' ') : std::string{};

        switch (type()) {
            case Type::Null:   out += "null"; break;
            case Type::Bool:   out += as_bool() ? "true" : "false"; break;
            case Type::Int:    out += std::to_string(as_int()); break;
            case Type::Real: {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.17g", as_double());
                out += buf;
                break;
            }
            case Type::String: escape_into(out, as_string()); break;
            case Type::Array: {
                const auto& a = as_array();
                if (a.empty()) { out += "[]"; break; }
                out.push_back('[');
                for (std::size_t i = 0; i < a.size(); ++i) {
                    if (i) out.push_back(',');
                    if (pretty) { out.push_back('\n'); out += pad; }
                    a[i].dump_into(out, indent, depth + 1);
                }
                if (pretty) { out.push_back('\n'); out += pad_end; }
                out.push_back(']');
                break;
            }
            case Type::Object: {
                const auto& o = as_object();
                if (o.empty()) { out += "{}"; break; }
                out.push_back('{');
                std::size_t i = 0;
                for (const auto& [k, val] : o) {
                    if (i++) out.push_back(',');
                    if (pretty) { out.push_back('\n'); out += pad; }
                    escape_into(out, k);
                    out.push_back(':');
                    if (pretty) out.push_back(' ');
                    val.dump_into(out, indent, depth + 1);
                }
                if (pretty) { out.push_back('\n'); out += pad_end; }
                out.push_back('}');
                break;
            }
        }
    }

    struct Parser {
        std::string_view s;
        std::size_t pos = 0;

        [[noreturn]] void fail(const char* msg) const {
            throw std::runtime_error(std::string("json: ") + msg +
                                     " at offset " + std::to_string(pos));
        }
        char peek() const { return pos < s.size() ? s[pos] : '\0'; }
        void skip_ws() {
            while (pos < s.size()) {
                char c = s[pos];
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos;
                else break;
            }
        }
        void expect(char c) {
            if (peek() != c) fail("unexpected character");
            ++pos;
        }

        Json parse_value() {
            skip_ws();
            char c = peek();
            switch (c) {
                case '{': return parse_object();
                case '[': return parse_array();
                case '"': return Json(parse_string());
                case 't': return parse_literal("true", Json(true));
                case 'f': return parse_literal("false", Json(false));
                case 'n': return parse_literal("null", Json(nullptr));
                default:
                    if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
                    fail("unexpected token");
            }
        }

        Json parse_literal(std::string_view lit, Json value) {
            if (s.substr(pos, lit.size()) != lit) fail("bad literal");
            pos += lit.size();
            return value;
        }

        Json parse_object() {
            expect('{');
            object o;
            skip_ws();
            if (peek() == '}') { ++pos; return Json(std::move(o)); }
            while (true) {
                skip_ws();
                std::string key = parse_string();
                skip_ws();
                expect(':');
                o.emplace(std::move(key), parse_value());
                skip_ws();
                char c = peek();
                if (c == ',') { ++pos; continue; }
                if (c == '}') { ++pos; break; }
                fail("expected ',' or '}'");
            }
            return Json(std::move(o));
        }

        Json parse_array() {
            expect('[');
            array a;
            skip_ws();
            if (peek() == ']') { ++pos; return Json(std::move(a)); }
            while (true) {
                a.push_back(parse_value());
                skip_ws();
                char c = peek();
                if (c == ',') { ++pos; continue; }
                if (c == ']') { ++pos; break; }
                fail("expected ',' or ']'");
            }
            return Json(std::move(a));
        }

        std::string parse_string() {
            expect('"');
            std::string out;
            while (pos < s.size()) {
                char c = s[pos++];
                if (c == '"') return out;
                if (c == '\\') {
                    if (pos >= s.size()) fail("bad escape");
                    char e = s[pos++];
                    switch (e) {
                        case '"':  out.push_back('"');  break;
                        case '\\': out.push_back('\\'); break;
                        case '/':  out.push_back('/');  break;
                        case 'n':  out.push_back('\n'); break;
                        case 't':  out.push_back('\t'); break;
                        case 'r':  out.push_back('\r'); break;
                        case 'b':  out.push_back('\b'); break;
                        case 'f':  out.push_back('\f'); break;
                        case 'u':  out += parse_unicode_escape(); break;
                        default: fail("bad escape");
                    }
                } else {
                    out.push_back(c);
                }
            }
            fail("unterminated string");
        }

        std::string parse_unicode_escape() {
            auto hex4 = [&]() -> uint32_t {
                if (pos + 4 > s.size()) fail("bad \\u escape");
                uint32_t cp = 0;
                for (int i = 0; i < 4; ++i) {
                    char h = s[pos++];
                    cp <<= 4;
                    if (h >= '0' && h <= '9')      cp |= (h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
                    else fail("bad hex digit");
                }
                return cp;
            };
            uint32_t cp = hex4();
            if (cp >= 0xD800 && cp <= 0xDBFF && pos + 6 <= s.size() &&
                s[pos] == '\\' && s[pos + 1] == 'u') {
                pos += 2;
                uint32_t lo = hex4();
                if (lo >= 0xDC00 && lo <= 0xDFFF)
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            // UTF-8 encode.
            std::string out;
            if (cp < 0x80) {
                out.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            return out;
        }

        Json parse_number() {
            std::size_t start = pos;
            bool is_real = false;
            if (peek() == '-') ++pos;
            while (pos < s.size()) {
                char c = s[pos];
                if (c >= '0' && c <= '9') { ++pos; }
                else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                    is_real = true;
                    ++pos;
                } else break;
            }
            std::string_view num = s.substr(start, pos - start);
            if (is_real) return Json(std::strtod(std::string(num).c_str(), nullptr));
            return Json(static_cast<int64_t>(std::strtoll(std::string(num).c_str(), nullptr, 10)));
        }
    };
};

}  // namespace wvc
