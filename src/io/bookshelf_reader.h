// BookShelf 格式解析器。
// 规范依据：work_report/05-开发执行规范.md §5.1
//
// 铁律 2：.pl 中的【左下角】坐标在此换算为 PlaceDB 的【中心】坐标，
// 全项目仅此处与 bookshelf_writer 各出现一次。
#pragma once

#include <string>
#include <vector>

namespace sp {

class PlaceDB;

struct BookshelfPaths {
    std::string dir;
    std::string nodes, nets, wts, pl, scl;
};

class BookshelfReader {
public:
    /// 读取 .aux 指向的整套文件并填充 db。失败抛 std::runtime_error。
    void read(const std::string& auxPath, PlaceDB& db);

    /// 仅读取 .pl 覆盖已有节点位置（后续里程碑的 --loadpl 用）。
    void readPlacement(const std::string& plPath, PlaceDB& db);

    const BookshelfPaths& paths() const { return paths_; }

private:
    BookshelfPaths paths_;

    /// pin -> net 下标。解析 .nets 时填充，交给 PlaceDB::finalizeCSR 建 CSR。
    std::vector<int> pinNet_;

    void readAux(const std::string& auxPath);
    void readScl(PlaceDB& db);
    void readNodes(PlaceDB& db);
    void readNets(PlaceDB& db);
    void readWts(PlaceDB& db);
};

}  // namespace sp
