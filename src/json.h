// Minimal JSON value that keeps object key order (game files must round-trip in order).
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class Json {
public:
    enum Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : t_(Bool), b_(b) {}
    Json(int v) : t_(Number), num_(v), numText_(std::to_string(v)) {}
    Json(int64_t v) : t_(Number), num_((double)v), numText_(std::to_string(v)) {}
    Json(double v);
    Json(const char* s) : t_(String), s_(s) {}
    Json(std::string s) : t_(String), s_(std::move(s)) {}

    static Json array() { Json j; j.t_ = Array; return j; }
    static Json object() { Json j; j.t_ = Object; return j; }
    static Json parse(const std::string& text);  // throws std::runtime_error

    Type type() const { return t_; }
    bool is_null() const { return t_ == Null; }
    bool is_string() const { return t_ == String; }
    bool is_object() const { return t_ == Object; }
    bool is_array() const { return t_ == Array; }
    bool is_number() const { return t_ == Number; }

    const std::string& str() const { return s_; }
    std::string& str() { return s_; }
    double num() const { return num_; }
    int64_t int64() const;  // exact for big ids (parsed from the original text, not the double)
    bool boolean() const { return b_; }

    // array
    std::vector<Json>& items() { return arr_; }
    const std::vector<Json>& items() const { return arr_; }
    void push(Json v) { if (t_ == Null) t_ = Array; arr_.push_back(std::move(v)); }
    size_t size() const { return t_ == Array ? arr_.size() : obj_.size(); }

    // object (ordered)
    std::vector<std::pair<std::string, Json>>& members() { return obj_; }
    const std::vector<std::pair<std::string, Json>>& members() const { return obj_; }
    const Json* find(const std::string& k) const;
    Json* find(const std::string& k);
    bool has(const std::string& k) const { return find(k) != nullptr; }
    Json& operator[](const std::string& k);  // inserts null if missing
    const Json& at(const std::string& k) const;
    std::string get_str(const std::string& k, const std::string& def = "") const;

    // indent < 0: compact. Non-ASCII written as raw UTF-8 (like ensure_ascii=False).
    std::string dump(int indent = -1) const;

private:
    void dump_to(std::string& out, int indent, int depth) const;
    Type t_ = Null;
    bool b_ = false;
    double num_ = 0;
    std::string numText_;  // original text so numbers round-trip exactly
    std::string s_;
    std::vector<Json> arr_;
    std::vector<std::pair<std::string, Json>> obj_;
    friend class JsonParser;
};

void json_escape(std::string& out, const std::string& s);
