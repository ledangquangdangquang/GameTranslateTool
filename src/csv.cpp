#include "csv.h"

#include <cctype>

namespace {
std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
}  // namespace

std::vector<std::vector<CsvField>> csv_parse(const std::string& t, char delim) {
    std::vector<std::vector<CsvField>> rows;
    std::vector<CsvField> row;
    size_t i = 0, n = t.size();
    while (i < n) {
        size_t fs = i;
        std::string val;
        if (t[i] == '"') {
            i++;
            for (;;) {
                if (i >= n) break;
                if (t[i] == '"') {
                    if (i + 1 < n && t[i + 1] == '"') { val += '"'; i += 2; continue; }
                    i++;
                    break;
                }
                val += t[i++];
            }
        } else {
            while (i < n && t[i] != delim && t[i] != '\r' && t[i] != '\n') val += t[i++];
        }
        row.push_back({val, fs, i});
        if (i < n && t[i] == delim) { i++; continue; }
        if (i < n && t[i] == '\r') i++;
        if (i < n && t[i] == '\n') i++;
        rows.push_back(std::move(row));
        row.clear();
    }
    if (!row.empty()) rows.push_back(std::move(row));
    return rows;
}

size_t csv_bom(const std::string& t) { return t.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0; }

int csv_find_column(const std::vector<std::vector<CsvField>>& rows, const std::string& name) {
    if (rows.empty()) return -1;
    std::string target = lower(name);
    for (size_t c = 0; c < rows[0].size(); c++)
        if (lower(rows[0][c].value) == target) return (int)c;
    return -1;
}

std::string csv_encode(const std::string& v, char delim) {
    if (v.find_first_of(std::string(1, delim) + "\"\r\n") == std::string::npos) return v;
    std::string o = "\"";
    for (char c : v) { if (c == '"') o += "\"\""; else o += c; }
    return o + "\"";
}
