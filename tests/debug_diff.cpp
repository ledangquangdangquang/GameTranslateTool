// Debug helper: where does SerializedFile::save() or a typetree round-trip diverge?
#include <algorithm>
#include <cstdio>

#include "../src/bundle.h"
#include "../src/serialized.h"
#include "../src/typetree.h"

static void diff(const Bytes& a, const Bytes& b, const char* what) {
    size_t n = std::min(a.size(), b.size()), i = 0;
    while (i < n && a[i] == b[i]) i++;
    printf("  %s: size %zu vs %zu, first diff at %zu\n", what, a.size(), b.size(), i);
}

static void dump_tree(const TTNode& n, int d, int maxd) {
    if (d > maxd) return;
    printf("%*s%s %s size=%d meta=%x\n", d * 2, "", n.type.c_str(), n.name.c_str(), n.byteSize, n.metaFlag);
    for (auto& c : n.children) dump_tree(c, d + 1, maxd);
}

static void check(const Bytes& data) {
    SerializedFile sf = SerializedFile::load(data);
    Bytes out = sf.save();
    diff(data, out, "save");
    std::vector<const ObjectInfo*> order;
    for (auto& o : sf.objects) order.push_back(&o);
    std::sort(order.begin(), order.end(), [](auto a, auto b) { return a->byteStart < b->byteStart; });
    int shown = 0;
    for (size_t k = 0; k + 1 < order.size() && shown < 3; k++) {
        uint64_t end = order[k]->byteStart + order[k]->byteSize;
        uint64_t aligned = (end + 7) / 8 * 8;
        if (aligned != order[k + 1]->byteStart) {
            printf("  gap: obj end %llu next start %llu\n", (unsigned long long)end,
                   (unsigned long long)order[k + 1]->byteStart);
            shown++;
        }
    }
    printf("  first obj start %llu, last end %llu, file %zu\n", (unsigned long long)order[0]->byteStart,
           (unsigned long long)(order.back()->byteStart + order.back()->byteSize), data.size());
    for (auto& o : sf.objects) {
        const TTNode* t = sf.tree_of(o);
        if (!t) continue;
        Bytes d = sf.object_data(o);
        Value v;
        try {
            Reader r(d, sf.bigEndian);
            v = tt_read(*t, d, sf.bigEndian);
            Bytes w = tt_write(*t, v, sf.bigEndian);
            if (w != d) {
                printf("  object %s:\n", sf.peek_name(o).c_str());
                diff(d, w, "roundtrip");
                dump_tree(*t, 0, 3);
                return;
            }
        } catch (const std::exception& e) {
            printf("  object %s: %s\n", sf.peek_name(o).c_str(), e.what());
            dump_tree(*t, 0, 3);
            return;
        }
    }
}

int main(int argc, char** argv) {
    Bytes data = read_file(u8path(argv[1]));
    if (Bundle::is_bundle(data)) {
        for (auto& n : Bundle::load(data).nodes)
            if (n.is_serialized()) check(n.data);
    } else check(data);
}
