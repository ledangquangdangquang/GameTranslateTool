#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

Json::Json(double v) : t_(Number), num_(v) {
    char buf[32];
    if (std::floor(v) == v && std::fabs(v) < 1e15) snprintf(buf, sizeof buf, "%.0f", v);
    else snprintf(buf, sizeof buf, "%.17g", v);
    numText_ = buf;
}

int64_t Json::int64() const {
    if (!numText_.empty() && numText_.find_first_of(".eE") == std::string::npos)
        return strtoll(numText_.c_str(), nullptr, 10);
    return (int64_t)num_;
}

const Json* Json::find(const std::string& k) const {
    for (auto& kv : obj_) if (kv.first == k) return &kv.second;
    return nullptr;
}
Json* Json::find(const std::string& k) {
    for (auto& kv : obj_) if (kv.first == k) return &kv.second;
    return nullptr;
}
Json& Json::operator[](const std::string& k) {
    if (t_ == Null) t_ = Object;
    if (Json* j = find(k)) return *j;
    obj_.emplace_back(k, Json());
    return obj_.back().second;
}
const Json& Json::at(const std::string& k) const {
    const Json* j = find(k);
    if (!j) throw std::runtime_error("thieu khoa JSON: " + k);
    return *j;
}
std::string Json::get_str(const std::string& k, const std::string& def) const {
    const Json* j = find(k);
    return j && j->is_string() ? j->str() : def;
}

// ---------------------------------------------------------------- parser
class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}
    Json run() {
        ws();
        Json v = value();
        ws();
        if (i_ != s_.size()) fail("thua ky tu sau JSON");
        return v;
    }

private:
    [[noreturn]] void fail(const char* m) {
        throw std::runtime_error(std::string("JSON loi: ") + m + " (vi tri " + std::to_string(i_) + ")");
    }
    void ws() { while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) i_++; }
    char peek() { return i_ < s_.size() ? s_[i_] : '\0'; }
    void expect(char c) { if (peek() != c) fail("ky tu khong mong doi"); i_++; }

    Json value() {
        char c = peek();
        if (c == '{') return object();
        if (c == '[') return array();
        if (c == '"') return Json(string());
        if (c == 't' && s_.compare(i_, 4, "true") == 0) { i_ += 4; return Json(true); }
        if (c == 'f' && s_.compare(i_, 5, "false") == 0) { i_ += 5; return Json(false); }
        if (c == 'n' && s_.compare(i_, 4, "null") == 0) { i_ += 4; return Json(); }
        if (c == '-' || (c >= '0' && c <= '9')) return number();
        fail("gia tri khong hop le");
    }
    Json object() {
        Json j = Json::object();
        expect('{');
        ws();
        if (peek() == '}') { i_++; return j; }
        for (;;) {
            ws();
            std::string k = string();
            ws();
            expect(':');
            ws();
            j.obj_.emplace_back(std::move(k), value());
            ws();
            if (peek() == ',') { i_++; continue; }
            expect('}');
            return j;
        }
    }
    Json array() {
        Json j = Json::array();
        expect('[');
        ws();
        if (peek() == ']') { i_++; return j; }
        for (;;) {
            ws();
            j.arr_.push_back(value());
            ws();
            if (peek() == ',') { i_++; continue; }
            expect(']');
            return j;
        }
    }
    Json number() {
        size_t st = i_;
        if (peek() == '-') i_++;
        while (i_ < s_.size() && strchr("0123456789.eE+-", s_[i_])) i_++;
        Json j;
        j.t_ = Json::Number;
        j.numText_ = s_.substr(st, i_ - st);
        j.num_ = strtod(j.numText_.c_str(), nullptr);
        return j;
    }
    static void put_utf8(std::string& o, uint32_t cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | cp >> 6); o += (char)(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | cp >> 12); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
        else { o += (char)(0xF0 | cp >> 18); o += (char)(0x80 | ((cp >> 12) & 63)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
    }
    uint32_t hex4() {
        if (i_ + 4 > s_.size()) fail("\\u thieu ky tu");
        uint32_t v = 0;
        for (int k = 0; k < 4; k++) {
            char c = s_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else fail("\\u sai");
        }
        return v;
    }
    std::string string() {
        expect('"');
        std::string o;
        for (;;) {
            if (i_ >= s_.size()) fail("chuoi chua dong");
            char c = s_[i_++];
            if (c == '"') return o;
            if (c != '\\') { o += c; continue; }
            if (i_ >= s_.size()) fail("escape cuoi chuoi");
            char e = s_[i_++];
            switch (e) {
                case '"': o += '"'; break;
                case '\\': o += '\\'; break;
                case '/': o += '/'; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'n': o += '\n'; break;
                case 'r': o += '\r'; break;
                case 't': o += '\t'; break;
                case 'u': {
                    uint32_t cp = hex4();
                    if (cp >= 0xD800 && cp < 0xDC00 && s_.compare(i_, 2, "\\u") == 0) {
                        i_ += 2;
                        uint32_t lo = hex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    put_utf8(o, cp);
                    break;
                }
                default: fail("escape khong hop le");
            }
        }
    }
    const std::string& s_;
    size_t i_ = 0;
};

Json Json::parse(const std::string& text) { return JsonParser(text).run(); }

// ---------------------------------------------------------------- writer
void json_escape(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof b, "\\u%04x", c);
                    out += b;
                } else out += (char)c;
        }
    }
    out += '"';
}

static void newline(std::string& out, int indent, int depth) {
    if (indent < 0) return;
    out += '\n';
    out.append((size_t)indent * depth, ' ');
}

void Json::dump_to(std::string& out, int indent, int depth) const {
    switch (t_) {
        case Null: out += "null"; break;
        case Bool: out += b_ ? "true" : "false"; break;
        case Number: out += numText_.empty() ? "0" : numText_; break;
        case String: json_escape(out, s_); break;
        case Array:
            if (arr_.empty()) { out += "[]"; break; }
            out += '[';
            for (size_t i = 0; i < arr_.size(); i++) {
                if (i) out += indent < 0 ? ", " : ",";
                newline(out, indent, depth + 1);
                arr_[i].dump_to(out, indent, depth + 1);
            }
            newline(out, indent, depth);
            out += ']';
            break;
        case Object:
            if (obj_.empty()) { out += "{}"; break; }
            out += '{';
            for (size_t i = 0; i < obj_.size(); i++) {
                if (i) out += indent < 0 ? ", " : ",";
                newline(out, indent, depth + 1);
                json_escape(out, obj_[i].first);
                out += ": ";
                obj_[i].second.dump_to(out, indent, depth + 1);
            }
            newline(out, indent, depth);
            out += '}';
            break;
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    dump_to(out, indent, 0);
    return out;
}
