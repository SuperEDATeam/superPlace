// BookShelf 输出。
// 铁律 2：PlaceDB 存【中心】坐标，.pl 要求【左下角】，
// 中心 -> 左下角的换算全项目仅此处出现一次。
#pragma once

#include <string>

namespace sp {

class PlaceDB;

class BookshelfWriter {
public:
    /// 仅写 .pl。path 为完整文件路径。
    static void writePl(const std::string& path, const PlaceDB& db);
};

}  // namespace sp
