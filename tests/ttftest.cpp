// Adds the missing Vietnamese letters to a .ttf (ttf_add_vietnamese) and writes the result:
//   ttftest <in.ttf> <out.ttf>
// g++ -std=c++17 -O2 -Isrc -o build/ttftest.exe tests/ttftest.cpp src/ttf.cpp src/bytes.cpp -static -municode -lgdi32
#include <cstdio>

#include "../src/ttf.h"

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::printf("ttftest <in.ttf> <out.ttf>\n");
        return 2;
    }
    Bytes ttf = read_file(argv[1]);
    std::printf("thieu %d chu tieng Viet\n", ttf_missing_vietnamese(ttf));
    bool ok = ttf_add_vietnamese(ttf, [](const std::string& s) { std::printf("%s\n", s.c_str()); });
    if (!ok) {
        std::printf("khong vá duoc\n");
        return 1;
    }
    write_file(argv[2], ttf);
    std::printf("sau khi va: thieu %d\n", ttf_missing_vietnamese(ttf));
    return 0;
}
