// 以给定 .pl 为输入，单独回放宏合法化（mLG）。
//
// 用途：复现修复段的问题。输出 .pl 里 mLG 之后的宏位置就是当时卡住的状态，
// 直接喂给 legalizeMacros 即可，不必重跑几分钟的 QP 与 mGP。
//   superplace-mlg-replay <design.aux> <layout.pl> <config.json>
#include <cstdio>

#include "db/place_db.h"
#include "io/bookshelf_reader.h"
#include "lg/macro_sa.h"
#include "util/config.h"
#include "util/metrics.h"

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <design.aux> <layout.pl> <config.json>\n", argv[0]);
        return 2;
    }
    sp::Config cfg;
    cfg.loadJson(argv[3]);
    sp::PlaceDB db;
    sp::BookshelfReader reader;
    reader.read(argv[1], db);
    // 输出 .pl 里 mLG 后的宏带 /FIXED；只取坐标，可移动标志沿用原设计
    const auto flags = db.node_flags;
    reader.readPlacement(argv[2], db);
    db.node_flags = flags;
    sp::MetricsSink sink;
    const sp::MacroSaResult r = sp::legalizeMacros(db, cfg, sink);
    std::printf("RESULT 宏 %d 个，重叠 %.6g -> SA 后 %.6g -> 最终 %.6g，连锁推挤移动 %d 个宏次\n",
                r.numMacros, r.overlapBefore, r.overlapAfterSa, r.overlapAfter, r.chained);
    return r.overlapAfter > 0.0 ? 1 : 0;
}
