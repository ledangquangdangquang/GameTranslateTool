// Machine translation (Google, one string per request - batching misaligns lines) with
// protection of tags, placeholders, line breaks, names and glossary terms.
#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>

struct Glossary {
    std::vector<std::string> keep;              // never translated (character names...)
    std::map<std::string, std::string> terms;    // fixed translation inside sentences
    std::map<std::string, std::string> whole;    // exact full-line translations (Yes -> Co)

    static Glossary load_or_default(const std::string& pathUtf8);
    void save(const std::string& pathUtf8) const;
};

// Which engine does the actual translation of one protected segment.
struct EngineConfig {
    enum Kind { Google, Local } kind = Google;
    int port = 8080;  // Local: llama-server (OpenAI-compatible) on 127.0.0.1:port
};

class Translator {
public:
    Translator(const Glossary& g, EngineConfig engine = {}, std::string from = "en", std::string to = "vi");
    // nullopt = network failure or the engine mangled a protected token (keep the line untranslated)
    std::optional<std::string> translate(const std::string& text);
    std::string last_error() const;  // why the last translate() returned nullopt

private:
    std::optional<std::string> segment(const std::string& seg);
    std::optional<std::string> translate_line(const std::string& text);  // tags left in the line: whole, then pieces
    std::optional<std::string> engine(const std::string& text, double temperature = 0.7);
    std::string translate_branches(const std::string& placeholder);  // SmartFormat {X:plural:card|cards}
    std::map<std::string, std::string> branchCache_;
    Glossary g_;
    EngineConfig eng_;
    bool netFail_ = false;
    std::string from_, to_;
    std::vector<std::pair<std::string, std::string>> terms_;  // longest first
};

std::optional<std::string> google_translate(const std::string& text, const std::string& from, const std::string& to);
// Hy-MT style prompt through llama-server /v1/chat/completions on localhost.
std::optional<std::string> local_translate(const std::string& text, const std::string& to, int port,
                                           double temperature = 0.7);
bool local_server_ready(int port);  // GET /health == 200

struct HttpResult {
    unsigned status = 0;  // 0 = connection failed
    std::string body;
};
HttpResult http_request(const wchar_t* method, const wchar_t* host, int port, bool https, const std::wstring& path,
                        const std::string& contentType, const std::string& body, int timeoutMs = 30000);

// Tags/placeholders that must survive translation: <...>, {0}, Scribble [c_red], SmartFormat
// {Cards:plural:card|cards} (whole expression; only its branch text may change)
std::vector<std::string> find_tags(const std::string& s);
bool same_tags(const std::string& a, const std::string& b);
// same_tags, plus no stray bracket characters ({Sát thương}, [3]) and no CJK/Hangul/Thai/Cyrillic
// word the source doesn't have; what a translation must pass to be kept
bool translation_ok(const std::string& src, const std::string& out);
