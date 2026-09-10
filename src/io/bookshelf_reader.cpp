#include "io/bookshelf_reader.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "db/place_db.h"
#include "util/logger.h"

namespace fs = std::filesystem;

namespace sp {
namespace {

// ------------------------------------------------------------------ 文本工具
std::string readWholeFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open: " + path);
    std::string buf;
    in.seekg(0, std::ios::end);
    const std::streamoff sz = in.tellg();
    in.seekg(0, std::ios::beg);
    buf.resize(static_cast<size_t>(sz));
    in.read(buf.data(), sz);
    buf.resize(static_cast<size_t>(in.gcount()));
    return buf;
}

inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/// 行迭代器：把整块缓冲按 '\n' 切开，返回不含换行的 view。
class LineScanner {
public:
    explicit LineScanner(const std::string& buf) : p_(buf.data()), end_(buf.data() + buf.size()) {}

    bool next(std::string_view& out) {
        if (p_ >= end_) return false;
        const char* lineBegin = p_;
        const char* nl = static_cast<const char*>(std::memchr(p_, '\n', static_cast<size_t>(end_ - p_)));
        const char* lineEnd = nl ? nl : end_;
        p_ = nl ? nl + 1 : end_;
        while (lineEnd > lineBegin && isSpace(lineEnd[-1])) --lineEnd;
        out = std::string_view(lineBegin, static_cast<size_t>(lineEnd - lineBegin));
        return true;
    }

private:
    const char* p_;
    const char* end_;
};

/// 跳过 BookShelf 的 "UCLA xxx 1.0" 头行、'#' 注释行与空行。
inline bool isSkippable(std::string_view line) {
    size_t i = 0;
    while (i < line.size() && isSpace(line[i])) ++i;
    if (i == line.size()) return true;                       // 空行
    if (line[i] == '#') return true;                         // 注释
    if (line.compare(i, 5, "UCLA ") == 0) return true;       // 版本头
    return false;
}

/// 按空白切分为 token（'\t' 与 ' ' 等价）。
void tokenize(std::string_view line, std::vector<std::string_view>& out) {
    out.clear();
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && isSpace(line[i])) ++i;
        if (i >= line.size()) break;
        const size_t b = i;
        while (i < line.size() && !isSpace(line[i])) ++i;
        out.emplace_back(line.data() + b, i - b);
    }
}

int toInt(std::string_view s) {
    int v = 0;
    const auto res = std::from_chars(s.data(), s.data() + s.size(), v);
    if (res.ec != std::errc()) {
        // 容错：形如 "12.000000" 的整数字段
        double d = 0;
        const auto r2 = std::from_chars(s.data(), s.data() + s.size(), d);
        if (r2.ec != std::errc()) throw std::runtime_error("bad integer: " + std::string(s));
        return static_cast<int>(d);
    }
    return v;
}

float toFloat(std::string_view s) {
    float v = 0.f;
    const auto res = std::from_chars(s.data(), s.data() + s.size(), v);
    if (res.ec != std::errc()) throw std::runtime_error("bad float: " + std::string(s));
    return v;
}

/// 取 "Key : Value" 形式中冒号后的第一个 token。找不到冒号返回 false。
bool valueAfterColon(const std::vector<std::string_view>& tok, size_t from, std::string_view& out) {
    for (size_t i = from; i + 1 < tok.size(); ++i) {
        if (tok[i] == ":") {
            out = tok[i + 1];
            return true;
        }
        // 形如 "Coordinate:459" 或 "Key:" 粘连的情况
        const size_t c = tok[i].find(':');
        if (c != std::string_view::npos && c + 1 < tok[i].size()) {
            out = tok[i].substr(c + 1);
            return true;
        }
    }
    return false;
}

/// BookShelf 方向字符串 -> 枚举（N/S/E/W/FN/FS/FE/FW）
uint8_t orientToCode(std::string_view s) {
    static const char* kNames[] = {"N", "E", "S", "W", "FN", "FE", "FS", "FW"};
    for (uint8_t i = 0; i < 8; ++i)
        if (s == kNames[i]) return i;
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------- .aux
void BookshelfReader::readAux(const std::string& auxPath) {
    const std::string buf = readWholeFile(auxPath);
    paths_.dir = fs::path(auxPath).parent_path().string();
    if (paths_.dir.empty()) paths_.dir = ".";

    LineScanner scanner(buf);
    std::string_view line;
    std::vector<std::string_view> tok;
    while (scanner.next(line)) {
        if (isSkippable(line)) continue;
        tokenize(line, tok);
        // 形如 "RowBasedPlacement : a.nodes a.nets a.wts a.pl a.scl"
        // 顺序不保证，按扩展名识别
        for (std::string_view t : tok) {
            if (t == ":" || t.find('.') == std::string_view::npos) continue;
            const std::string name(t);
            const std::string ext = fs::path(name).extension().string();
            const std::string full = (fs::path(paths_.dir) / name).string();
            if      (ext == ".nodes") paths_.nodes = full;
            else if (ext == ".nets")  paths_.nets  = full;
            else if (ext == ".wts")   paths_.wts   = full;
            else if (ext == ".pl")    paths_.pl    = full;
            else if (ext == ".scl")   paths_.scl   = full;
        }
    }

    if (paths_.nodes.empty() || paths_.nets.empty() || paths_.pl.empty() || paths_.scl.empty())
        throw std::runtime_error("aux file missing required entries: " + auxPath);
}

// ---------------------------------------------------------------------- .scl
void BookshelfReader::readScl(PlaceDB& db) {
    const std::string buf = readWholeFile(paths_.scl);
    LineScanner scanner(buf);
    std::string_view line;
    std::vector<std::string_view> tok;

    bool inRow = false;
    PlaceDB::SiteRow row;
    float siteWidth = 1.f, siteSpacing = 0.f;

    while (scanner.next(line)) {
        if (isSkippable(line)) continue;
        tokenize(line, tok);
        if (tok.empty()) continue;

        if (tok[0] == "CoreRow") {
            inRow = true;
            row = PlaceDB::SiteRow{};
            siteWidth = 1.f;
            siteSpacing = 0.f;
            continue;
        }
        if (tok[0] == "End") {
            if (inRow) {
                row.step = (siteSpacing > 0.f) ? siteSpacing : siteWidth;
                db.rows.push_back(row);
                inRow = false;
            }
            continue;
        }
        if (!inRow) continue;   // NumRows 等外层字段不需要

        std::string_view v;
        if (tok[0] == "Coordinate") {
            if (valueAfterColon(tok, 0, v)) row.ly = toFloat(v);
        } else if (tok[0] == "Height") {
            if (valueAfterColon(tok, 0, v)) row.height = toFloat(v);
        } else if (tok[0] == "Sitewidth") {
            if (valueAfterColon(tok, 0, v)) siteWidth = toFloat(v);
        } else if (tok[0] == "Sitespacing") {
            if (valueAfterColon(tok, 0, v)) siteSpacing = toFloat(v);
        } else if (tok[0] == "SubrowOrigin") {
            // 注意：本行含两个键值对
            //   "SubrowOrigin : 459 <TAB> NumSites : 10692"
            if (valueAfterColon(tok, 0, v)) row.lx = toFloat(v);
            for (size_t i = 1; i < tok.size(); ++i) {
                if (tok[i] == "NumSites" || tok[i].compare(0, 8, "NumSites") == 0) {
                    std::string_view nv;
                    if (valueAfterColon(tok, i, nv)) row.numSites = toInt(nv);
                    break;
                }
            }
        }
    }

    // 兼容缺少 End 标记的写法
    if (inRow) {
        row.step = (siteSpacing > 0.f) ? siteSpacing : siteWidth;
        db.rows.push_back(row);
    }
    if (db.rows.empty()) throw std::runtime_error("no CoreRow found in " + paths_.scl);
}

// -------------------------------------------------------------------- .nodes
void BookshelfReader::readNodes(PlaceDB& db) {
    const std::string buf = readWholeFile(paths_.nodes);
    LineScanner scanner(buf);
    std::string_view line;
    std::vector<std::string_view> tok;

    int declaredNodes = -1, declaredTerminals = -1;

    while (scanner.next(line)) {
        if (isSkippable(line)) continue;
        tokenize(line, tok);
        if (tok.empty()) continue;

        if (tok[0] == "NumNodes") {
            std::string_view v;
            if (valueAfterColon(tok, 0, v)) {
                declaredNodes = toInt(v);
                db.reserveNodes(declaredNodes);
            }
            continue;
        }
        if (tok[0] == "NumTerminals") {
            std::string_view v;
            if (valueAfterColon(tok, 0, v)) declaredTerminals = toInt(v);
            continue;
        }
        if (tok.size() < 3) continue;

        // "name width height [terminal|terminal_NI]"
        uint8_t flags = 0;
        if (tok.size() >= 4) {
            if (tok[3] == "terminal") {
                flags |= F_FIXED;
            } else if (tok[3] == "terminal_NI") {
                flags |= F_FIXED | F_NI;
            }
        }
        db.addNode(std::string(tok[0]), toFloat(tok[1]), toFloat(tok[2]), flags);
    }

    db.numNodes = static_cast<int>(db.node_x.size());
    if (declaredNodes >= 0 && declaredNodes != db.numNodes)
        SP_WARN("NumNodes declares %d but parsed %d", declaredNodes, db.numNodes);

    int fixedCount = 0;
    for (int i = 0; i < db.numNodes; ++i)
        if (db.isFixed(i)) ++fixedCount;
    if (declaredTerminals >= 0 && declaredTerminals != fixedCount)
        SP_WARN("NumTerminals declares %d but parsed %d", declaredTerminals, fixedCount);
}

// --------------------------------------------------------------------- .nets
void BookshelfReader::readNets(PlaceDB& db) {
    const std::string buf = readWholeFile(paths_.nets);
    LineScanner scanner(buf);
    std::string_view line;
    std::vector<std::string_view> tok;

    // name -> index，仅解析期使用（partitionNodes 之后即失效）
    std::unordered_map<std::string_view, int> nameToIdx;
    nameToIdx.reserve(static_cast<size_t>(db.numNodes) * 2);
    for (int i = 0; i < db.numNodes; ++i) nameToIdx.emplace(db.node_name[i], i);

    int declaredNets = -1, declaredPins = -1;
    int curNet = -1;
    int remaining = 0;

    while (scanner.next(line)) {
        if (isSkippable(line)) continue;
        tokenize(line, tok);
        if (tok.empty()) continue;

        if (tok[0] == "NumNets") {
            std::string_view v;
            if (valueAfterColon(tok, 0, v)) {
                declaredNets = toInt(v);
                db.net_name.reserve(declaredNets);
                db.net_weight.reserve(declaredNets);
            }
            continue;
        }
        if (tok[0] == "NumPins") {
            std::string_view v;
            if (valueAfterColon(tok, 0, v)) {
                declaredPins = toInt(v);
                db.pin2node.reserve(declaredPins);
                db.pin_offset_x.reserve(declaredPins);
                db.pin_offset_y.reserve(declaredPins);
                pinNet_.reserve(declaredPins);
            }
            continue;
        }
        if (tok[0] == "NetDegree") {
            std::string_view v;
            if (!valueAfterColon(tok, 0, v)) throw std::runtime_error("malformed NetDegree line");
            remaining = toInt(v);
            curNet = static_cast<int>(db.net_name.size());
            // 网络名是冒号后的第二个 token（部分变体省略）
            std::string netName = "n" + std::to_string(curNet);
            for (size_t i = 0; i + 2 < tok.size(); ++i) {
                if (tok[i] == ":") {
                    if (i + 2 < tok.size()) netName = std::string(tok[i + 2]);
                    break;
                }
            }
            db.net_name.push_back(std::move(netName));
            db.net_weight.push_back(1.0f);
            continue;
        }

        if (curNet < 0 || remaining <= 0) continue;

        // 引脚行： "nodeName [I|O|B] : offsetX offsetY"
        auto it = nameToIdx.find(tok[0]);
        if (it == nameToIdx.end())
            throw std::runtime_error("net references unknown node: " + std::string(tok[0]));

        float ox = 0.f, oy = 0.f;
        for (size_t i = 1; i < tok.size(); ++i) {
            if (tok[i] != ":") continue;
            if (i + 2 < tok.size()) {
                ox = toFloat(tok[i + 1]);
                oy = toFloat(tok[i + 2]);
            }
            break;
        }

        db.pin2node.push_back(it->second);
        db.pin_offset_x.push_back(ox);
        db.pin_offset_y.push_back(oy);
        pinNet_.push_back(curNet);
        --remaining;
    }

    db.numNets = static_cast<int>(db.net_name.size());
    db.numPins = static_cast<int>(db.pin2node.size());
    if (declaredNets >= 0 && declaredNets != db.numNets)
        SP_WARN("NumNets declares %d but parsed %d", declaredNets, db.numNets);
    if (declaredPins >= 0 && declaredPins != db.numPins)
        SP_WARN("NumPins declares %d but parsed %d", declaredPins, db.numPins);
}

// ----------------------------------------------------------------------- .pl
void BookshelfReader::readPlacement(const std::string& plPath, PlaceDB& db) {
    const std::string buf = readWholeFile(plPath);
    LineScanner scanner(buf);
    std::string_view line;
    std::vector<std::string_view> tok;

    std::unordered_map<std::string_view, int> nameToIdx;
    nameToIdx.reserve(static_cast<size_t>(db.numNodes) * 2);
    for (int i = 0; i < db.numNodes; ++i) nameToIdx.emplace(db.node_name[i], i);

    while (scanner.next(line)) {
        if (isSkippable(line)) continue;
        tokenize(line, tok);
        if (tok.size() < 3) continue;

        auto it = nameToIdx.find(tok[0]);
        if (it == nameToIdx.end()) continue;   // .pl 可能含 db 中不存在的条目，跳过
        const int i = it->second;

        // 铁律 2：.pl 存的是【左下角】，PlaceDB 存【中心】，此处是全项目唯一的换算点之一
        const float llx = toFloat(tok[1]);
        const float lly = toFloat(tok[2]);
        db.node_x[i] = llx + 0.5f * db.node_w[i];
        db.node_y[i] = lly + 0.5f * db.node_h[i];

        for (size_t k = 3; k < tok.size(); ++k) {
            if (tok[k] == ":") {
                if (k + 1 < tok.size()) db.node_orient[i] = orientToCode(tok[k + 1]);
            } else if (tok[k].find("FIXED") != std::string_view::npos) {
                db.node_flags[i] |= F_FIXED;
            }
        }
    }
}

// ---------------------------------------------------------------------- .wts
void BookshelfReader::readWts(PlaceDB& db) {
    db.net_weight.assign(static_cast<size_t>(db.numNets), 1.0f);
    if (paths_.wts.empty() || !fs::exists(paths_.wts)) {
        SP_INFO("no .wts file, all net weights default to 1.0");
        return;
    }

    const std::string buf = readWholeFile(paths_.wts);
    LineScanner scanner(buf);
    std::string_view line;
    std::vector<std::string_view> tok;

    std::unordered_map<std::string_view, int> netToIdx;
    netToIdx.reserve(static_cast<size_t>(db.numNets) * 2);
    for (int k = 0; k < db.numNets; ++k) netToIdx.emplace(db.net_name[k], k);

    int applied = 0;
    while (scanner.next(line)) {
        if (isSkippable(line)) continue;
        tokenize(line, tok);
        if (tok.size() < 2) continue;
        auto it = netToIdx.find(tok[0]);
        if (it == netToIdx.end()) continue;
        db.net_weight[it->second] = toFloat(tok[1]);
        ++applied;
    }
    SP_INFO(".wts applied %d net weights (rest default 1.0)", applied);
}

// -------------------------------------------------------------------- 主流程
void BookshelfReader::read(const std::string& auxPath, PlaceDB& db) {
    pinNet_.clear();

    readAux(auxPath);
    SP_INFO("aux: %s", auxPath.c_str());

    readScl(db);
    db.computeRegions();          // 先得到 rowHeight，供宏单元判定使用
    SP_INFO("scl: %zu rows, row height %.0f", db.rows.size(), db.rowHeight);

    readNodes(db);
    SP_INFO("nodes: %d objects", db.numNodes);

    readNets(db);
    SP_INFO("nets: %d nets, %d pins", db.numNets, db.numPins);

    readPlacement(paths_.pl, db);
    readWts(db);

    // 宏单元判定：不可移动的终端不算宏（05 §5.1.2）
    for (int i = 0; i < db.numNodes; ++i) {
        if (!db.isFixed(i) && db.node_h[i] > db.rowHeight * 1.5f)
            db.node_flags[i] |= F_MACRO;
    }

    // 重排为 [可移动 | 固定]，并同步 pin2node（pinNet 不受影响）
    db.partitionNodes();

    db.finalizeCSR(pinNet_);
    db.computeRegions();          // 节点就绪后重算 chipRegion
}

}  // namespace sp
