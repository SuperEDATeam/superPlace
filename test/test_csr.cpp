// CSR 双向索引正确性验证。
// 思路：不信任 CSR，改用独立的朴素方式（vector<vector>）重建一份参考索引，
// 再与 CSR 切片逐项比对。这样 CSR 的两趟法一旦写错就会暴露。
#include <algorithm>
#include <string>
#include <vector>

#include "db/module_ref.h"
#include "db/place_db.h"
#include "io/bookshelf_reader.h"
#include "test_util.h"

int main(int argc, char** argv) {
    const std::string aux =
        (argc > 1) ? argv[1] : std::string(SP_TEST_DATA_DIR) + "/thin1/thin1.aux";

    sp::PlaceDB db;
    sp::BookshelfReader reader;
    reader.read(aux, db);

    // ---------------------------------------------- CSR 结构性不变量
    CHECK_EQ(static_cast<int>(db.net2pin_start.size()), db.numNets + 1);
    CHECK_EQ(static_cast<int>(db.node2pin_start.size()), db.numNodes + 1);
    CHECK_EQ(db.net2pin_start.front(), 0);
    CHECK_EQ(db.node2pin_start.front(), 0);
    CHECK_EQ(db.net2pin_start.back(), db.numPins);
    CHECK_EQ(db.node2pin_start.back(), db.numPins);
    CHECK_EQ(static_cast<int>(db.flat_net2pin.size()), db.numPins);
    CHECK_EQ(static_cast<int>(db.flat_node2pin.size()), db.numPins);

    // start 数组必须单调不减
    bool monotonic = true;
    for (size_t k = 1; k < db.net2pin_start.size(); ++k)
        if (db.net2pin_start[k] < db.net2pin_start[k - 1]) monotonic = false;
    for (size_t i = 1; i < db.node2pin_start.size(); ++i)
        if (db.node2pin_start[i] < db.node2pin_start[i - 1]) monotonic = false;
    CHECK_TRUE(monotonic);

    // 每个 pin 恰好出现一次
    std::vector<int> seenNet(db.numPins, 0), seenNode(db.numPins, 0);
    for (int p : db.flat_net2pin) seenNet[p]++;
    for (int p : db.flat_node2pin) seenNode[p]++;
    bool exactlyOnce = true;
    for (int p = 0; p < db.numPins; ++p)
        if (seenNet[p] != 1 || seenNode[p] != 1) exactlyOnce = false;
    CHECK_TRUE(exactlyOnce);

    // ------------------------------------- node->pin：与朴素重建的参考比对
    std::vector<std::vector<int>> refNode(db.numNodes);
    for (int p = 0; p < db.numPins; ++p) refNode[db.pin2node[p]].push_back(p);

    bool nodeCsrOk = true;
    for (int i = 0; i < db.numNodes; ++i) {
        sp::ModuleRef m(db, i);
        std::vector<int> got(m.pins().begin(), m.pins().end());
        std::vector<int> want = refNode[i];
        std::sort(got.begin(), got.end());
        std::sort(want.begin(), want.end());
        if (got != want) {
            nodeCsrOk = false;
            break;
        }
    }
    CHECK_TRUE(nodeCsrOk);

    // ------------------------------------- net->pin：反查 pin 的归属自洽性
    // flat_net2pin 中位于 net k 区间的每个 pin，其 pin2node 必须落在合法范围
    bool netCsrOk = true;
    int degreeSum = 0;
    for (int k = 0; k < db.numNets; ++k) {
        sp::NetRef n(db, k);
        degreeSum += n.degree();
        for (int p : n.pins()) {
            if (p < 0 || p >= db.numPins) { netCsrOk = false; break; }
            const int node = db.pin2node[p];
            if (node < 0 || node >= db.numNodes) { netCsrOk = false; break; }
        }
    }
    CHECK_TRUE(netCsrOk);
    CHECK_EQ(degreeSum, db.numPins);

    // ------------------------------------------------------------ 区域自洽
    CHECK_TRUE(db.coreRegion.valid());
    CHECK_TRUE(db.chipRegion.lx <= db.coreRegion.lx);
    CHECK_TRUE(db.chipRegion.hx >= db.coreRegion.hx);
    CHECK_TRUE(db.rowHeight > 0.f);

    return sptest::summary("test_csr");
}
