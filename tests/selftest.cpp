// Round-trip checks of the core readers on real game files:
//   selftest <file.assets|file.bundle> ...
// For each SerializedFile: save() with no edits must equal the input; every object with a
// typetree must survive read->write unchanged.
#include <cstdio>

#include "../src/bundle.h"
#include "../src/serialized.h"
#include "../src/typetree.h"

static int check_serialized(const Bytes& data, const std::string& label) {
    SerializedFile sf = SerializedFile::load(data);
    int bad = 0, typed = 0;
    if (sf.save() != data) {
        printf("  [FAIL] %s: save() khac file goc\n", label.c_str());
        bad++;
    }
    for (auto& o : sf.objects) {
        const TTNode* tree = sf.tree_of(o);
        if (!tree) continue;
        typed++;
        Value v;
        if (!tt_read_checked(*tree, sf.object_data(o), sf.bigEndian, v)) {
            if (bad < 5) printf("  [FAIL] round-trip object %lld class %d %s\n", (long long)o.pathId, o.classID,
                                sf.peek_name(o).c_str());
            bad++;
        }
    }
    printf("  %s: v%u unity %s, %zu objects, typetree=%d (%d checked), loi=%d\n", label.c_str(), sf.version,
           sf.unityVersion.c_str(), sf.objects.size(), sf.enableTypeTree, typed, bad);
    return bad;
}

int main(int argc, char** argv) {
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        printf("%s\n", argv[i]);
        try {
            Bytes data = read_file(u8path(argv[i]));
            if (Bundle::is_bundle(data)) {
                Bundle b = Bundle::load(data);
                Bundle b2 = Bundle::load(b.save());
                if (b2.nodes.size() != b.nodes.size()) { printf("  [FAIL] bundle save/load\n"); bad++; }
                for (size_t k = 0; k < b.nodes.size(); k++) {
                    if (b2.nodes[k].data != b.nodes[k].data) { printf("  [FAIL] node %zu khac\n", k); bad++; }
                    if (b.nodes[k].is_serialized()) bad += check_serialized(b.nodes[k].data, b.nodes[k].path);
                }
            } else {
                bad += check_serialized(data, "assets");
            }
        } catch (const std::exception& e) {
            printf("  [LOI] %s\n", e.what());
            bad++;
        }
    }
    printf(bad ? "\nCO LOI: %d\n" : "\nOK het\n", bad);
    return bad ? 1 : 0;
}
