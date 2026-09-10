// 句柄视图：只持有 PlaceDB 引用 + 索引，无存储、无堆分配、全 inline。
// 用途：解析、IO、绘图、调试、演示。
// 铁律 3：禁止用于热路径——热路径必须直接操作 PlaceDB 的裸数组。
// 规范依据：work_report/05-开发执行规范.md §3.3
#pragma once

#include "db/place_db.h"

namespace sp {

/// CSR 切片，可 range-for。
class PinRange {
public:
    PinRange(const int* b, const int* e) : b_(b), e_(e) {}
    const int* begin() const { return b_; }
    const int* end()   const { return e_; }
    int size()  const { return static_cast<int>(e_ - b_); }
    bool empty() const { return b_ == e_; }
    int operator[](int k) const { return b_[k]; }

private:
    const int* b_;
    const int* e_;
};

class ModuleRef {
public:
    ModuleRef(const PlaceDB& db, int i) : db_(&db), i_(i) {}

    int index() const { return i_; }
    const std::string& name() const { return db_->node_name[i_]; }

    float x()      const { return db_->node_x[i_]; }
    float y()      const { return db_->node_y[i_]; }
    float width()  const { return db_->node_w[i_]; }
    float height() const { return db_->node_h[i_]; }
    Rect  box()    const { return db_->box(i_); }
    double area()  const { return db_->area(i_); }

    bool isMacro()  const { return db_->isMacro(i_); }
    bool isFixed()  const { return db_->isFixed(i_); }
    bool isFiller() const { return db_->isFiller(i_); }
    bool isNI()     const { return db_->isNI(i_); }

    PinRange pins() const {
        return {db_->flat_node2pin.data() + db_->node2pin_start[i_],
                db_->flat_node2pin.data() + db_->node2pin_start[i_ + 1]};
    }

private:
    const PlaceDB* db_;
    int i_;
};

// 铁律 3：句柄必须保持轻量。
static_assert(sizeof(ModuleRef) <= 16, "ModuleRef must stay a thin handle");
static_assert(sizeof(PinRange) <= 16, "PinRange must stay a thin view");

class NetRef {
public:
    NetRef(const PlaceDB& db, int k) : db_(&db), k_(k) {}

    int index() const { return k_; }
    const std::string& name() const { return db_->net_name[k_]; }
    float weight() const { return db_->net_weight[k_]; }
    int degree() const { return db_->netDegree(k_); }

    PinRange pins() const {
        return {db_->flat_net2pin.data() + db_->net2pin_start[k_],
                db_->flat_net2pin.data() + db_->net2pin_start[k_ + 1]};
    }

private:
    const PlaceDB* db_;
    int k_;
};

}  // namespace sp
