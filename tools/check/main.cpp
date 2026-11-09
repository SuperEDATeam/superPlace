// superplace-check：布局合法性检查器（05 §4.6，M5 验收用）。
//
// 独立性是这个工具存在的意义：它与布局器共享任何代码，就会共享那份代码的 bug，
// 检查随之失去意义。所以这里不链接 libsuperplace，连 BookShelf 解析器也自己写一份——
// 05 §4.6 允许复用 io/，但合法化器读入的行信息若被 io/ 读错，复用的检查器也会跟着读错。
//
// 判定口径（与 Abacus 合法化器的目标一致，但实现完全独立）：
//   - 对象分类只看 .nodes：terminal / terminal_NI 是固定物体，其余是可移动节点。
//     .pl 里的 /FIXED 不作数——superPlace 写出时会给 mLG 钉死的宏也加上 /FIXED。
//   - 可移动节点高度不超过行高的是标准单元，否则按宏处理。
//   - 标准单元：底边落在某个子行上、整体在该子行的水平范围内、左边缘对齐该子行的 site 网格。
//   - 宏：底边落在某个子行的 y 上、左边缘对齐该行的 site 网格（mLG 按行与 site 栅格对齐宏）。
//   - 所有可移动节点都不得越出 core（全部子行的外包矩形）。
//   - 重叠只统计「可移动-可移动」与「可移动-固定」；固定物体之间的重叠是输入数据自带的，不计入。
//   - terminal_NI 与零面积节点不参与任何几何检查。
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

// ------------------------------------------------------------------ 文本解析

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "superplace-check: 无法读取 %s\n", path.c_str());
        std::exit(2);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// 把一行切成空白分隔的词；冒号单独成词（"NumSites:10692" 也能切开）。
void tokenize(std::string_view line, std::vector<std::string_view>& out) {
    out.clear();
    size_t i = 0;
    const size_t n = line.size();
    while (i < n) {
        while (i < n && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
        if (i >= n) break;
        if (line[i] == ':') {
            out.push_back(line.substr(i, 1));
            ++i;
            continue;
        }
        const size_t b = i;
        while (i < n && line[i] != ' ' && line[i] != '\t' && line[i] != '\r' && line[i] != ':') ++i;
        out.push_back(line.substr(b, i - b));
    }
}

template <typename Fn>
void forEachLine(const std::string& text, Fn&& fn) {
    std::vector<std::string_view> tok;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string_view line(text.data() + pos, end - pos);
        pos = end + 1;
        const size_t hash = line.find('#');
        if (hash != std::string_view::npos) line = line.substr(0, hash);
        tokenize(line, tok);
        if (!tok.empty()) fn(tok);
    }
}

double toNum(std::string_view s, const char* what) {
    std::string tmp(s);
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(tmp.c_str(), &end);
    if (end == tmp.c_str() || errno != 0) {
        std::fprintf(stderr, "superplace-check: %s 不是数值：'%s'\n", what, tmp.c_str());
        std::exit(2);
    }
    return v;
}

/// 返回第 i 个词之后、紧跟冒号的那个值（"Key : value"）；没有则返回空。
std::string_view valueAfterColon(const std::vector<std::string_view>& tok, size_t i) {
    if (i + 2 < tok.size() && tok[i + 1] == ":") return tok[i + 2];
    return {};
}

// ------------------------------------------------------------------ 数据

struct Node {
    std::string name;
    double w = 0, h = 0;       // .nodes 中的原始尺寸
    bool terminal = false;     // terminal 或 terminal_NI
    bool ni = false;           // terminal_NI
    bool placed = false;       // .pl 中出现过
    double llx = 0, lly = 0;   // 左下角
    int orient = 0;            // 0..7：N E S W FN FE FS FW
    // 派生
    double ew = 0, eh = 0;     // 按方向旋转后的实际宽高
};

struct Subrow {
    double ly, height, lx, step;
    long numSites;
    double hx() const { return lx + step * static_cast<double>(numSites); }
};

struct PinRef {
    int node;
    double dx, dy;   // 相对节点中心
};

struct Design {
    std::string name;
    std::vector<Node> nodes;
    std::unordered_map<std::string, int> index;
    std::vector<Subrow> rows;
    std::vector<int> netStart;   // CSR
    std::vector<PinRef> pins;
};

int orientCode(std::string_view s) {
    static const char* kNames[] = {"N", "E", "S", "W", "FN", "FE", "FS", "FW"};
    for (int k = 0; k < 8; ++k)
        if (s == kNames[k]) return k;
    return -1;
}

std::string dirOf(const std::string& path) {
    const size_t p = path.find_last_of('/');
    return p == std::string::npos ? std::string() : path.substr(0, p + 1);
}

void parseNodes(const std::string& path, Design& d) {
    forEachLine(readFile(path), [&](const std::vector<std::string_view>& t) {
        if (t[0] == "UCLA" || t[0] == "NumNodes" || t[0] == "NumTerminals") return;
        if (t.size() < 3) return;
        Node n;
        n.name = std::string(t[0]);
        n.w = toNum(t[1], "节点宽度");
        n.h = toNum(t[2], "节点高度");
        if (t.size() >= 4) {
            if (t[3] == "terminal") n.terminal = true;
            if (t[3] == "terminal_NI") n.terminal = n.ni = true;
        }
        d.index.emplace(n.name, static_cast<int>(d.nodes.size()));
        d.nodes.push_back(std::move(n));
    });
}

void parsePl(const std::string& path, Design& d) {
    long unknown = 0;
    forEachLine(readFile(path), [&](const std::vector<std::string_view>& t) {
        if (t[0] == "UCLA" || t.size() < 3) return;
        const auto it = d.index.find(std::string(t[0]));
        if (it == d.index.end()) {
            ++unknown;
            return;
        }
        Node& n = d.nodes[static_cast<size_t>(it->second)];
        n.llx = toNum(t[1], "x 坐标");
        n.lly = toNum(t[2], "y 坐标");
        n.placed = true;
        for (size_t i = 3; i < t.size(); ++i) {
            const int o = orientCode(t[i]);
            if (o >= 0) {
                n.orient = o;
                break;
            }
        }
    });
    if (unknown > 0)
        std::fprintf(stderr, "superplace-check: 警告：.pl 中有 %ld 个节点不在 .nodes 里，已忽略\n",
                     unknown);
}

void parseScl(const std::string& path, Design& d) {
    bool inRow = false;
    double ly = 0, height = 0, siteW = 1, siteS = 0;
    std::vector<std::pair<double, long>> origins;   // (SubrowOrigin, NumSites)
    auto flush = [&]() {
        const double step = siteS > 0 ? siteS : siteW;
        for (const auto& [lx, ns] : origins) d.rows.push_back({ly, height, lx, step, ns});
        origins.clear();
    };
    forEachLine(readFile(path), [&](const std::vector<std::string_view>& t) {
        if (t[0] == "CoreRow") {
            if (inRow) flush();
            inRow = true;
            ly = height = siteS = 0;
            siteW = 1;
            return;
        }
        if (t[0] == "End") {
            if (inRow) flush();
            inRow = false;
            return;
        }
        if (!inRow) return;
        const std::string_view v = valueAfterColon(t, 0);
        if (t[0] == "Coordinate" && !v.empty()) ly = toNum(v, "Coordinate");
        else if (t[0] == "Height" && !v.empty()) height = toNum(v, "Height");
        else if (t[0] == "Sitewidth" && !v.empty()) siteW = toNum(v, "Sitewidth");
        else if (t[0] == "Sitespacing" && !v.empty()) siteS = toNum(v, "Sitespacing");
        else if (t[0] == "SubrowOrigin" && !v.empty()) {
            long ns = 0;
            for (size_t i = 1; i < t.size(); ++i)
                if (t[i] == "NumSites" && !valueAfterColon(t, i).empty())
                    ns = static_cast<long>(toNum(valueAfterColon(t, i), "NumSites"));
            origins.emplace_back(toNum(v, "SubrowOrigin"), ns);
        }
    });
    if (inRow) flush();
}

void parseNets(const std::string& path, Design& d) {
    long remaining = 0;
    long unknownPins = 0;
    forEachLine(readFile(path), [&](const std::vector<std::string_view>& t) {
        if (t[0] == "UCLA" || t[0] == "NumNets" || t[0] == "NumPins") return;
        if (t[0] == "NetDegree") {
            const std::string_view v = valueAfterColon(t, 0);
            remaining = v.empty() ? 0 : static_cast<long>(toNum(v, "NetDegree"));
            d.netStart.push_back(static_cast<int>(d.pins.size()));
            return;
        }
        if (remaining <= 0) return;
        --remaining;
        const auto it = d.index.find(std::string(t[0]));
        if (it == d.index.end()) {
            ++unknownPins;
            return;
        }
        PinRef p{it->second, 0, 0};
        // "node dir : dx dy"
        for (size_t i = 1; i + 2 < t.size(); ++i) {
            if (t[i] == ":") {
                p.dx = toNum(t[i + 1], "引脚偏移");
                p.dy = toNum(t[i + 2], "引脚偏移");
                break;
            }
        }
        d.pins.push_back(p);
    });
    d.netStart.push_back(static_cast<int>(d.pins.size()));
    if (unknownPins > 0)
        std::fprintf(stderr, "superplace-check: 警告：.nets 中有 %ld 个引脚引用了未知节点\n",
                     unknownPins);
}

Design load(const std::string& auxPath, const std::string& plOverride) {
    Design d;
    std::string nodes, nets, pl, scl;
    forEachLine(readFile(auxPath), [&](const std::vector<std::string_view>& t) {
        for (const std::string_view w : t) {
            const std::string f(w);
            auto ends = [&](const char* ext) {
                const size_t n = std::strlen(ext);
                return f.size() > n && f.compare(f.size() - n, n, ext) == 0;
            };
            if (ends(".nodes")) nodes = f;
            else if (ends(".nets")) nets = f;
            else if (ends(".pl")) pl = f;
            else if (ends(".scl")) scl = f;
        }
    });
    if (nodes.empty() || pl.empty() || scl.empty()) {
        std::fprintf(stderr, "superplace-check: %s 缺少 .nodes / .pl / .scl\n", auxPath.c_str());
        std::exit(2);
    }
    const std::string dir = dirOf(auxPath);
    {
        std::string base = auxPath.substr(dir.size());
        const size_t dot = base.rfind('.');
        d.name = dot == std::string::npos ? base : base.substr(0, dot);
    }
    parseNodes(dir + nodes, d);
    parsePl(plOverride.empty() ? dir + pl : plOverride, d);
    parseScl(dir + scl, d);
    if (!nets.empty()) parseNets(dir + nets, d);
    return d;
}

// ------------------------------------------------------------------ 几何

/// 方向对尺寸与引脚偏移的作用。E/W/FE/FW 旋转 90°，宽高互换。
void orientOffset(int o, double dx, double dy, double* ox, double* oy) {
    switch (o) {
        case 0: *ox = dx;  *oy = dy;  break;   // N
        case 1: *ox = dy;  *oy = -dx; break;   // E：顺时针 90°
        case 2: *ox = -dx; *oy = -dy; break;   // S
        case 3: *ox = -dy; *oy = dx;  break;   // W：逆时针 90°
        case 4: *ox = -dx; *oy = dy;  break;   // FN：关于竖直轴镜像
        case 5: *ox = dy;  *oy = dx;  break;   // FE：FN 后顺时针 90°
        case 6: *ox = dx;  *oy = -dy; break;   // FS：关于水平轴镜像
        default: *ox = -dy; *oy = -dx; break;  // FW：FN 后逆时针 90°
    }
}

bool rotated(int o) { return o == 1 || o == 3 || o == 5 || o == 7; }

enum Kind { kCell = 0, kMacro = 1, kFixed = 2, kIgnored = 3 };

struct Options {
    double eps = 1e-3;
    long maxPairs = 20000000;   // 重叠对数上限：全堆在一点的布局会有 O(n²) 对，数到上限即停
    int examples = 5;
};

struct Violation {
    std::string text;
};

struct Report {
    long movable = 0, cells = 0, macros = 0, fixed = 0, ignored = 0, unplaced = 0;
    long overlapPairs = 0;
    double overlapArea = 0;
    long pairsByType[5] = {0, 0, 0, 0, 0};   // cell-cell, cell-macro, cell-fixed, macro-macro, macro-fixed
    bool overlapTruncated = false;
    long outOfCore = 0, offRow = 0, offSite = 0;
    double hpwl = 0;
    double rowHeight = 0;
    double coreLx = 0, coreLy = 0, coreHx = 0, coreHy = 0;
    std::vector<Violation> exOverlap, exCore, exRow, exSite;
};

double modeHeight(const std::vector<Subrow>& rows) {
    std::unordered_map<long long, int> cnt;
    double best = 0;
    int bestN = 0;
    for (const Subrow& r : rows) {
        const long long key = std::llround(r.height * 1000.0);
        const int c = ++cnt[key];
        if (c > bestN) {
            bestN = c;
            best = r.height;
        }
    }
    return best;
}

void addExample(std::vector<Violation>& v, int limit, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));
void addExample(std::vector<Violation>& v, int limit, const char* fmt, ...) {
    if (static_cast<int>(v.size()) >= limit) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    v.push_back({buf});
}

Report check(Design& d, const Options& opt) {
    Report rep;
    const double eps = opt.eps;
    if (d.rows.empty()) {
        std::fprintf(stderr, "superplace-check: .scl 中没有任何行\n");
        std::exit(2);
    }

    // ---- core 与行高
    rep.coreLx = rep.coreLy = 1e300;
    rep.coreHx = rep.coreHy = -1e300;
    for (const Subrow& r : d.rows) {
        rep.coreLx = std::min(rep.coreLx, r.lx);
        rep.coreLy = std::min(rep.coreLy, r.ly);
        rep.coreHx = std::max(rep.coreHx, r.hx());
        rep.coreHy = std::max(rep.coreHy, r.ly + r.height);
    }
    rep.rowHeight = modeHeight(d.rows);

    // 子行按 (ly, lx) 排序，便于按底边 y 查找
    std::vector<Subrow> rows = d.rows;
    std::sort(rows.begin(), rows.end(), [](const Subrow& a, const Subrow& b) {
        return a.ly != b.ly ? a.ly < b.ly : a.lx < b.lx;
    });
    auto rowsAt = [&](double y) {
        auto lo = std::lower_bound(rows.begin(), rows.end(), y - eps,
                                   [](const Subrow& r, double v) { return r.ly < v; });
        auto hi = lo;
        while (hi != rows.end() && hi->ly <= y + eps) ++hi;
        return std::make_pair(lo, hi);
    };

    // ---- 分类
    const int n = static_cast<int>(d.nodes.size());
    std::vector<int> kind(static_cast<size_t>(n), kIgnored);
    for (int i = 0; i < n; ++i) {
        Node& nd = d.nodes[static_cast<size_t>(i)];
        nd.ew = rotated(nd.orient) ? nd.h : nd.w;
        nd.eh = rotated(nd.orient) ? nd.w : nd.h;
        if (nd.ni || nd.ew <= 0 || nd.eh <= 0) {
            ++rep.ignored;
            continue;
        }
        if (!nd.placed) {
            if (!nd.terminal) ++rep.unplaced;
            continue;
        }
        if (nd.terminal) {
            kind[static_cast<size_t>(i)] = kFixed;
            ++rep.fixed;
        } else if (nd.eh <= rep.rowHeight + eps) {
            kind[static_cast<size_t>(i)] = kCell;
            ++rep.cells;
        } else {
            kind[static_cast<size_t>(i)] = kMacro;
            ++rep.macros;
        }
    }
    rep.movable = rep.cells + rep.macros;
    if (rep.unplaced > 0) {
        std::fprintf(stderr, "superplace-check: %ld 个可移动节点在 .pl 中没有位置\n", rep.unplaced);
        std::exit(2);
    }

    // ---- 逐节点：越界、在行上、对齐 site
    for (int i = 0; i < n; ++i) {
        const int k = kind[static_cast<size_t>(i)];
        if (k != kCell && k != kMacro) continue;
        const Node& nd = d.nodes[static_cast<size_t>(i)];
        const double urx = nd.llx + nd.ew, ury = nd.lly + nd.eh;
        if (nd.llx < rep.coreLx - eps || urx > rep.coreHx + eps || nd.lly < rep.coreLy - eps ||
            ury > rep.coreHy + eps) {
            ++rep.outOfCore;
            addExample(rep.exCore, opt.examples, "%s (%.3f, %.3f) 尺寸 %.3f×%.3f", nd.name.c_str(),
                       nd.llx, nd.lly, nd.ew, nd.eh);
        }

        const auto [lo, hi] = rowsAt(nd.lly);
        const Subrow* host = nullptr;
        for (auto it = lo; it != hi; ++it) {
            if (k == kCell) {
                if (nd.llx >= it->lx - eps && urx <= it->hx() + eps && nd.eh <= it->height + eps) {
                    host = &*it;
                    break;
                }
            } else if (nd.llx >= it->lx - eps && nd.llx < it->hx()) {
                host = &*it;
                break;
            }
        }
        if (!host && k == kMacro && lo != hi) host = &*lo;   // 宏只要求底边在行上
        if (!host) {
            ++rep.offRow;
            addExample(rep.exRow, opt.examples, "%s%s (%.3f, %.3f) 尺寸 %.3f×%.3f",
                       k == kMacro ? "[宏] " : "", nd.name.c_str(), nd.llx, nd.lly, nd.ew, nd.eh);
            continue;
        }
        const double s = (nd.llx - host->lx) / host->step;
        if (std::fabs(s - std::round(s)) * host->step > eps) {
            ++rep.offSite;
            addExample(rep.exSite, opt.examples, "%s%s x=%.3f（所在子行 lx=%.3f，site=%.3f）",
                       k == kMacro ? "[宏] " : "", nd.name.c_str(), nd.llx, host->lx, host->step);
        }
    }

    // ---- 重叠：按行高切成水平条带，条带内按 x 扫描
    //
    // 每个对象登记到它覆盖的所有条带；一对对象只在两者共同覆盖的第一个条带里计数，避免重复。
    std::vector<int> objs;
    double y0 = 1e300;
    for (int i = 0; i < n; ++i) {
        const int k = kind[static_cast<size_t>(i)];
        if (k == kIgnored) continue;
        objs.push_back(i);
        y0 = std::min(y0, d.nodes[static_cast<size_t>(i)].lly);
    }
    const double bandH = rep.rowHeight > 0 ? rep.rowHeight : 1.0;
    auto bandOf = [&](double y) { return static_cast<long>(std::floor((y - y0) / bandH)); };
    std::vector<long> b0(static_cast<size_t>(n), 0), b1(static_cast<size_t>(n), 0);
    long maxBand = 0;
    for (const int i : objs) {
        const Node& nd = d.nodes[static_cast<size_t>(i)];
        b0[static_cast<size_t>(i)] = bandOf(nd.lly + eps);
        b1[static_cast<size_t>(i)] = std::max(b0[static_cast<size_t>(i)], bandOf(nd.lly + nd.eh - eps));
        maxBand = std::max(maxBand, b1[static_cast<size_t>(i)]);
    }
    // 条带成员用 CSR 存，避免为每个条带分配一个 vector
    std::vector<long> start(static_cast<size_t>(maxBand + 2), 0);
    for (const int i : objs)
        for (long b = b0[static_cast<size_t>(i)]; b <= b1[static_cast<size_t>(i)]; ++b)
            ++start[static_cast<size_t>(b + 1)];
    for (long b = 0; b <= maxBand; ++b) start[static_cast<size_t>(b + 1)] += start[static_cast<size_t>(b)];
    std::vector<int> member(static_cast<size_t>(start.back()));
    {
        std::vector<long> cur(start.begin(), start.end() - 1);
        for (const int i : objs)
            for (long b = b0[static_cast<size_t>(i)]; b <= b1[static_cast<size_t>(i)]; ++b)
                member[static_cast<size_t>(cur[static_cast<size_t>(b)]++)] = i;
    }

    auto pairType = [](int ka, int kb) {
        if (ka > kb) std::swap(ka, kb);
        if (ka == kCell) return kb == kCell ? 0 : kb == kMacro ? 1 : 2;
        return kb == kMacro ? 3 : 4;   // macro-macro / macro-fixed
    };
    static const char* kPairName[] = {"单元-单元", "单元-宏", "单元-固定", "宏-宏", "宏-固定"};

    std::vector<int> active;
    for (long b = 0; b <= maxBand && !rep.overlapTruncated; ++b) {
        auto first = member.begin() + start[static_cast<size_t>(b)];
        auto last = member.begin() + start[static_cast<size_t>(b + 1)];
        std::sort(first, last, [&](int a, int c) {
            const double xa = d.nodes[static_cast<size_t>(a)].llx, xc = d.nodes[static_cast<size_t>(c)].llx;
            return xa != xc ? xa < xc : a < c;
        });
        active.clear();
        for (auto it = first; it != last; ++it) {
            const int a = *it;
            const Node& na = d.nodes[static_cast<size_t>(a)];
            active.erase(std::remove_if(active.begin(), active.end(),
                                        [&](int c) {
                                            const Node& nc = d.nodes[static_cast<size_t>(c)];
                                            return nc.llx + nc.ew <= na.llx + eps;
                                        }),
                         active.end());
            const int ka = kind[static_cast<size_t>(a)];
            for (const int c : active) {
                const int kc = kind[static_cast<size_t>(c)];
                if (ka == kFixed && kc == kFixed) continue;
                if (std::max(b0[static_cast<size_t>(a)], b0[static_cast<size_t>(c)]) != b) continue;
                const Node& nc = d.nodes[static_cast<size_t>(c)];
                const double ix = std::min(na.llx + na.ew, nc.llx + nc.ew) - std::max(na.llx, nc.llx);
                const double iy = std::min(na.lly + na.eh, nc.lly + nc.eh) - std::max(na.lly, nc.lly);
                if (ix <= eps || iy <= eps) continue;
                ++rep.overlapPairs;
                rep.overlapArea += ix * iy;
                const int t = pairType(ka, kc);
                ++rep.pairsByType[t];
                addExample(rep.exOverlap, opt.examples, "[%s] %s 与 %s，重叠 %.3f×%.3f", kPairName[t],
                           nc.name.c_str(), na.name.c_str(), ix, iy);
                if (rep.overlapPairs >= opt.maxPairs) {
                    rep.overlapTruncated = true;
                    break;
                }
            }
            if (rep.overlapTruncated) break;
            active.push_back(a);
        }
    }

    // ---- HPWL：引脚 = 节点中心 + 按方向变换后的偏移；度数 < 2 的 net 不计
    for (size_t k = 0; k + 1 < d.netStart.size(); ++k) {
        const int s = d.netStart[k], e = d.netStart[k + 1];
        if (e - s < 2) continue;
        double minX = 1e300, maxX = -1e300, minY = 1e300, maxY = -1e300;
        for (int p = s; p < e; ++p) {
            const PinRef& pr = d.pins[static_cast<size_t>(p)];
            const Node& nd = d.nodes[static_cast<size_t>(pr.node)];
            double ox = 0, oy = 0;
            orientOffset(nd.orient, pr.dx, pr.dy, &ox, &oy);
            const double x = nd.llx + 0.5 * nd.ew + ox, y = nd.lly + 0.5 * nd.eh + oy;
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
        }
        rep.hpwl += (maxX - minX) + (maxY - minY);
    }
    return rep;
}

void printExamples(const char* title, const std::vector<Violation>& v) {
    if (v.empty()) return;
    std::printf("  %s（前 %zu 个）:\n", title, v.size());
    for (const Violation& x : v) std::printf("    %s\n", x.text.c_str());
}

int usage() {
    std::fprintf(stderr,
                 "用法: superplace-check <design.aux> [placement.pl] [--eps E] [--examples N]\n"
                 "                       [--max-pairs N]\n\n"
                 "  不给 placement.pl 时检查 .aux 自带的 .pl。\n"
                 "  --eps E         坐标比较容差（默认 1e-3）\n"
                 "  --examples N    每类违规最多列出几个例子（默认 5）\n"
                 "  --max-pairs N   重叠对数数到 N 即停止（默认 2e7）\n\n"
                 "退出码：0 = PASS，1 = FAIL，2 = 参数或输入文件错误。\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::string aux, pl;
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") return usage();
        if (a == "--eps" && i + 1 < argc) {
            opt.eps = std::atof(argv[++i]);
        } else if (a == "--examples" && i + 1 < argc) {
            opt.examples = std::atoi(argv[++i]);
        } else if (a == "--max-pairs" && i + 1 < argc) {
            opt.maxPairs = std::atol(argv[++i]);
        } else if (a.rfind("--", 0) == 0) {
            std::fprintf(stderr, "superplace-check: 未知参数 %s\n", a.c_str());
            return usage();
        } else if (aux.empty()) {
            aux = a;
        } else if (pl.empty()) {
            pl = a;
        } else {
            return usage();
        }
    }
    if (aux.empty()) return usage();

    Design d = load(aux, pl);
    const Report r = check(d, opt);
    const bool pass = r.overlapPairs == 0 && r.outOfCore == 0 && r.offRow == 0 && r.offSite == 0;

    std::printf("superplace-check: %s  (%s)\n", d.name.c_str(),
                pl.empty() ? "使用 .aux 自带的 .pl" : pl.c_str());
    std::printf("Nodes:          %ld movable (%ld cells, %ld macros), %ld fixed, %ld ignored\n",
                r.movable, r.cells, r.macros, r.fixed, r.ignored);
    std::printf("Rows:           %zu subrows, row height %g, core (%g, %g)-(%g, %g)\n", d.rows.size(),
                r.rowHeight, r.coreLx, r.coreLy, r.coreHx, r.coreHy);
    std::printf("Overlap:        %ld%s pairs, %.6g area\n", r.overlapPairs,
                r.overlapTruncated ? "+" : "", r.overlapArea);
    if (r.overlapPairs > 0)
        std::printf("                单元-单元 %ld，单元-宏 %ld，单元-固定 %ld，宏-宏 %ld，宏-固定 %ld%s\n",
                    r.pairsByType[0], r.pairsByType[1], r.pairsByType[2], r.pairsByType[3],
                    r.pairsByType[4], r.overlapTruncated ? "（达到 --max-pairs 上限，已停止计数）" : "");
    std::printf("Out of core:    %ld\n", r.outOfCore);
    std::printf("Off-row:        %ld\n", r.offRow);
    std::printf("Off-site:       %ld\n", r.offSite);
    std::printf("HPWL:           %.10g\n", r.hpwl);
    std::printf("Result:         %s\n", pass ? "PASS" : "FAIL");
    if (!pass) {
        printExamples("重叠", r.exOverlap);
        printExamples("越出 core", r.exCore);
        printExamples("不在行上", r.exRow);
        printExamples("未对齐 site", r.exSite);
    }
    return pass ? 0 : 1;
}
