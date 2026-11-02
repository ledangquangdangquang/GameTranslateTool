#include "bytes.h"

#include <cstdio>

Bytes read_file(const fs::path& p) {
    FILE* f = _wfopen(p.wstring().c_str(), L"rb");
    if (!f) throw std::runtime_error("khong mo duoc file: " + pathstr(p));
    Bytes b;
    _fseeki64(f, 0, SEEK_END);
    long long n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    b.resize((size_t)n);
    if (n && fread(b.data(), 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        throw std::runtime_error("doc file loi: " + pathstr(p));
    }
    fclose(f);
    return b;
}

void write_file(const fs::path& p, const Bytes& data) {
    fs::path tmp = p;
    tmp += ".tmp";
    FILE* f = _wfopen(tmp.wstring().c_str(), L"wb");
    if (!f) throw std::runtime_error("khong ghi duoc file: " + pathstr(tmp));
    if (!data.empty() && fwrite(data.data(), 1, data.size(), f) != data.size()) {
        fclose(f);
        throw std::runtime_error("ghi file loi (het dung luong?): " + pathstr(tmp));
    }
    fclose(f);
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(p, ec);
        fs::rename(tmp, p);
    }
}

std::string read_text(const fs::path& p) {
    Bytes b = read_file(p);
    std::string s(b.begin(), b.end());
    if (s.size() >= 3 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF) s.erase(0, 3);
    return s;
}
