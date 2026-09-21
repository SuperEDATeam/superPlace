// 课堂测试 1：按指定格式输出 BookShelf 解析结果。
//
// 这是一个**独立二进制**（superplace-quiz），链接现有的 libsuperplace，
// 不改动 superplace-cli 的任何行为——所需数据早已由解析器算出并通过 test_parser 验证，
// 本工具只负责按测试要求的格式重新排版。
//
// ============================ 关于 net degree 分箱 ============================
// 测试要求的口径是「字面理解的闭区间」：
//     1 / 2 / 3-10 / 11-100 / >100  =>  1348 / 117104 / 87085 / 15603 / 2
// 这与 awk 对 adaptec1.nets 的独立复算完全一致，本工具用 DbStats::naturalBins()。
//
// 注意不要与课程资料《3 布局数据文件解析》的 DATABASE SUMMARY 混淆：
// 那里写的是 3-10 (86566) / 11-100 (17470)，实际对应的边界是 <10 / <100，
// 即标签整体差一位、且 degree==1 的 net 被并进了标着 "3-10" 的那一箱
// （85218 + 1348 = 86566）。本测试要求的是前者。
// ============================================================================
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "db/db_stats.h"
#include "db/place_db.h"
#include "io/bookshelf_reader.h"
#include "util/logger.h"

namespace fs = std::filesystem;

namespace {

std::string baseName(const std::string& path) {
    return path.empty() ? std::string() : fs::path(path).filename().string();
}

/// 按课堂测试 1 要求的格式输出。auxArg 原样回显用户在命令行给出的路径。
void writeReport(std::ostream& os, const sp::PlaceDB& db, const sp::BookshelfPaths& paths,
                 const std::string& auxArg) {
    const sp::DbStats s = sp::DbStats::compute(db);
    const std::array<int, 5> bins = s.naturalBins();

    char buf[256];
    auto line = [&](const char* fmt, auto... args) {
        std::snprintf(buf, sizeof(buf), fmt, args...);
        os << buf << '\n';
    };

    // ------------------------------------------------------------ 读取过程
    os << "Use BOOKSHELF placement format\n";
    line("ReadAUXFile: %s", auxArg.c_str());

    line("ReadSCLFile: %s", baseName(paths.scl).c_str());
    line("CoreRegion: lower left: (%.0f,%.0f) to upper right: (%.0f,%.0f)", s.coreLx, s.coreLy,
         s.coreHx, s.coreHy);
    line("NumRows: %d", s.rowCount);

    line("ReadNodesFile: %s", baseName(paths.nodes).c_str());
    line("NumModules: %d", s.objectCount);
    line("NumNodes: %d", s.nodeCount);
    line("Terminals: %d", s.fixedCount);

    line("ReadNetsFile: %s", baseName(paths.nets).c_str());
    line("Nets: %d", s.netCount);
    line("Pins: %d", s.pinCount);
    line("Max net degree= %d", s.maxNetDegree);
    line("total pin number= %d", s.pinCount);

    line("ReadPLFile: %s", baseName(paths.pl).c_str());

    // -------------------------------------------------------------- 汇总
    os << "--------------SUMMARIES--------------\n";
    os << "Area:\n";
    line("Core Area: %.0f", s.coreArea);
    line("Cell Area: %.0f (cellArea / coreArea = %.2f%%)", s.movableArea,
         s.cellAreaRatio() * 100.0);
    line("Movable Area: %.0f (movableArea / coreArea = %.2f%%)", s.movableArea,
         s.cellAreaRatio() * 100.0);
    line("Fixed Area: %.0f (fixedArea / coreArea = %.2f%%)", s.fixedArea,
         s.fixedAreaRatio() * 100.0);
    line("Fixed Area in Core: %.0f (fixedAreaInCore / coreArea = %.2f%%)", s.fixedAreaInCore,
         s.fixedInCoreRatio() * 100.0);

    line("There are %d nets (has 1 pins)", bins[0]);
    line("There are %d nets (has 2 pins)", bins[1]);
    line("There are %d nets (has 3-10 pins)", bins[2]);
    line("There are %d nets (has 11-100 pins)", bins[3]);
    line("There are %d nets (has >100 pins)", bins[4]);
}

/// 与课堂测试 1 给出的 adaptec1 标准答案逐项核对。返回不一致的项数。
int verifyAgainstExpected(const sp::PlaceDB& db) {
    const sp::DbStats s = sp::DbStats::compute(db);
    const std::array<int, 5> bins = s.naturalBins();

    struct Check {
        const char* name;
        double got, want;
    };
    const Check checks[] = {
        {"CoreRegion.lx",      s.coreLx,            459},
        {"CoreRegion.ly",      s.coreLy,            459},
        {"CoreRegion.hx",      s.coreHx,            11151},
        {"CoreRegion.hy",      s.coreHy,            11139},
        {"NumRows",            double(s.rowCount),  890},
        {"NumModules",         double(s.objectCount), 211447},
        {"NumNodes",           double(s.nodeCount), 210904},
        {"Terminals",          double(s.fixedCount), 543},
        {"Nets",               double(s.netCount),  221142},
        {"Pins",              double(s.pinCount),  944053},
        {"Max net degree",     double(s.maxNetDegree), 2271},
        {"Core Area",          s.coreArea,          114190560},
        {"Cell Area",          s.movableArea,       37286292},
        {"Fixed Area",         s.fixedArea,         64093992},
        {"Fixed Area in Core", s.fixedAreaInCore,   49164072},
        {"nets with 1 pin",    double(bins[0]),     1348},
        {"nets with 2 pins",   double(bins[1]),     117104},
        {"nets with 3-10",     double(bins[2]),     87085},
        {"nets with 11-100",   double(bins[3]),     15603},
        {"nets with >100",     double(bins[4]),     2},
    };

    int bad = 0;
    std::fprintf(stderr, "\n--- verify against expected (adaptec1) ---\n");
    for (const Check& c : checks) {
        const bool ok = std::abs(c.got - c.want) < 0.5;
        if (!ok) ++bad;
        std::fprintf(stderr, "  %-20s %14.0f  %s\n", c.name, c.got,
                     ok ? "OK" : "MISMATCH");
    }
    // 百分比单独比（保留两位）
    struct Pct { const char* name; double got, want; };
    const Pct pcts[] = {
        {"cellArea ratio %",       s.cellAreaRatio() * 100.0,     32.65},
        {"fixedArea ratio %",      s.fixedAreaRatio() * 100.0,    56.13},
        {"fixedInCore ratio %",    s.fixedInCoreRatio() * 100.0,  43.05},
    };
    for (const Pct& p : pcts) {
        const bool ok = std::abs(p.got - p.want) < 0.005;
        if (!ok) ++bad;
        std::fprintf(stderr, "  %-20s %14.2f  %s\n", p.name, p.got, ok ? "OK" : "MISMATCH");
    }
    std::fprintf(stderr, bad == 0 ? "  => ALL %d ITEMS MATCH\n" : "  => %d MISMATCHES\n",
                 bad == 0 ? int(sizeof(checks) / sizeof(checks[0]) + 3) : bad);
    return bad;
}

void usage() {
    std::puts(
        "superplace-quiz — 课堂测试 1：按指定格式输出 BookShelf 解析结果\n"
        "\n"
        "Usage:\n"
        "  superplace-quiz <design.aux> [options]\n"
        "\n"
        "Options:\n"
        "  -o <file>    同时写入文件（默认只打印到 stdout）\n"
        "  --verify     与 adaptec1 标准答案逐项核对（结果打到 stderr）\n"
        "  -h, --help   显示本帮助\n"
        "\n"
        "Example:\n"
        "  ./build/superplace-quiz test_data/adaptec1/adaptec1.aux --verify\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string auxArg, outFile;
    bool verify = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a == "--verify") {
            verify = true;
        } else if (a == "-o") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -o needs a file path\n");
                return 2;
            }
            outFile = argv[++i];
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "error: unknown option %s\n", a.c_str());
            return 2;
        } else {
            auxArg = a;
        }
    }

    if (auxArg.empty()) {
        usage();
        return 2;
    }

    // 解析器自身的 INFO 日志会污染格式化输出，这里压到 ERROR 级
    sp::Logger::get().setLevel(sp::LogLevel::kError);

    sp::PlaceDB db;
    sp::BookshelfReader reader;
    try {
        reader.read(auxArg, db);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }

    std::ostringstream report;
    writeReport(report, db, reader.paths(), auxArg);
    std::cout << report.str();

    if (!outFile.empty()) {
        std::ofstream f(outFile);
        if (!f) {
            std::fprintf(stderr, "error: cannot write %s\n", outFile.c_str());
            return 1;
        }
        f << report.str();
        std::fprintf(stderr, "\n(report also written to %s)\n", outFile.c_str());
    }

    if (verify) return verifyAgainstExpected(db) == 0 ? 0 : 1;
    return 0;
}
