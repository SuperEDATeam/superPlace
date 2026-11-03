// T13：kindOf 判定顺序（06 §3.5）：Filler > Fixed > Macro > Cell；
// F_NI 位只标记零面积引脚，不改变类别。
#include "test_util.h"

#include "viz/scene.h"

using namespace viz;

void testKind() {
    CHECK_EQ(kindOf(0), Kind::Cell);
    CHECK_EQ(kindOf(1), Kind::Macro);                 // F_MACRO
    CHECK_EQ(kindOf(2), Kind::Fixed);                 // F_FIXED
    CHECK_EQ(kindOf(4), Kind::Filler);                // F_FILLER
    // 同时置位时按优先级：FILLER 压过 MACRO，FIXED 压过 MACRO
    CHECK_EQ(kindOf(4 | 1), Kind::Filler);
    CHECK_EQ(kindOf(2 | 1), Kind::Fixed);
    CHECK_EQ(kindOf(4 | 2 | 1), Kind::Filler);
    // F_NI 单独/叠加都不改类别（跳过由渲染与索引负责）
    CHECK_EQ(kindOf(8), Kind::Cell);
    CHECK_EQ(kindOf(8 | 1), Kind::Macro);
    CHECK_EQ(kindOf(8 | 4), Kind::Filler);
}
