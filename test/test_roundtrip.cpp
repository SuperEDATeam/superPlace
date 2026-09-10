// .pl 写出/读回往返一致性。
// 铁律 2 的回归测试：中心<->左下角的两次换算必须严格互逆。
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "db/place_db.h"
#include "io/bookshelf_reader.h"
#include "io/bookshelf_writer.h"
#include "test_util.h"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    const std::string aux =
        (argc > 1) ? argv[1] : std::string(SP_TEST_DATA_DIR) + "/adaptec1/adaptec1.aux";

    sp::PlaceDB db;
    sp::BookshelfReader reader;
    reader.read(aux, db);

    const std::vector<float> x0 = db.node_x;
    const std::vector<float> y0 = db.node_y;
    const std::vector<uint8_t> flags0 = db.node_flags;

    const fs::path tmp = fs::temp_directory_path() / "superplace_roundtrip.pl";
    sp::BookshelfWriter::writePl(tmp.string(), db);

    // 读回到同一个 db（readPlacement 按名字覆盖位置）
    reader.readPlacement(tmp.string(), db);

    int posMismatch = 0, flagMismatch = 0;
    for (int i = 0; i < db.numNodes; ++i) {
        if (db.node_x[i] != x0[i] || db.node_y[i] != y0[i]) ++posMismatch;
        if (db.node_flags[i] != flags0[i]) ++flagMismatch;
    }
    CHECK_EQ(posMismatch, 0);
    CHECK_EQ(flagMismatch, 0);

    // 连续两次写出应逐字节一致（铁律 7：可复现）
    const fs::path tmp2 = fs::temp_directory_path() / "superplace_roundtrip2.pl";
    sp::BookshelfWriter::writePl(tmp2.string(), db);
    CHECK_EQ(fs::file_size(tmp), fs::file_size(tmp2));

    std::FILE* f1 = std::fopen(tmp.c_str(), "rb");
    std::FILE* f2 = std::fopen(tmp2.c_str(), "rb");
    bool identical = (f1 && f2);
    if (identical) {
        std::vector<char> b1(1 << 16), b2(1 << 16);
        size_t n1 = 0, n2 = 0;
        do {
            n1 = std::fread(b1.data(), 1, b1.size(), f1);
            n2 = std::fread(b2.data(), 1, b2.size(), f2);
            if (n1 != n2 || std::memcmp(b1.data(), b2.data(), n1) != 0) {
                identical = false;
                break;
            }
        } while (n1 > 0);
    }
    if (f1) std::fclose(f1);
    if (f2) std::fclose(f2);
    CHECK_TRUE(identical);

    fs::remove(tmp);
    fs::remove(tmp2);
    return sptest::summary("test_roundtrip");
}
