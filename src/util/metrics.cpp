#include "util/metrics.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "util/logger.h"
#include "util/timer.h"

namespace fs = std::filesystem;

namespace sp {

void MetricsSink::setOutputDir(const std::string& dir) {
    out_dir_ = dir;
    std::error_code ec;
    fs::create_directories(dir, ec);
}

void MetricsSink::beginStage(const std::string& stage) {
    stage_stack_.push_back(stage);
    stages_.emplace_back(stage, std::vector<IterMetrics>{});
}

void MetricsSink::push(const IterMetrics& m) {
    if (stage_stack_.empty()) beginStage("unnamed");
    // 定位到栈顶阶段对应的那组。不能直接用 stages_.back()——子阶段结束后它仍是
    // 子阶段那一组，此时外层阶段再 push 会把数据记到已结束的子阶段名下。
    const std::string& cur = stage_stack_.back();
    for (auto it = stages_.rbegin(); it != stages_.rend(); ++it) {
        if (it->first == cur) {
            it->second.push_back(m);
            return;
        }
    }
    beginStage(cur);
    stages_.back().second.push_back(m);
}

void MetricsSink::endStage(double elapsed_ms) {
    if (!stage_stack_.empty()) {
        durations_[stage_stack_.back()] = elapsed_ms;
        stage_stack_.pop_back();
    }
    flushStageCsv();
}

void MetricsSink::recordDuration(const std::string& label, double ms) {
    durations_[label] = ms;
}

void MetricsSink::setSummary(const std::string& key, double value) {
    summary_num_[key] = value;
}

void MetricsSink::setSummary(const std::string& key, const std::string& value) {
    summary_str_[key] = value;
}

void MetricsSink::flushStageCsv() const {
    if (out_dir_.empty()) return;
    const fs::path csv = fs::path(out_dir_) / "metrics.csv";
    std::ofstream out(csv);
    if (!out) {
        SP_WARN("cannot write %s", csv.c_str());
        return;
    }
    out << "stage,iter,hpwl,overflow,lambda,gamma,step_size,"
           "grad_norm_wl,grad_norm_den,max_displacement,elapsed_ms\n";
    out << std::setprecision(10);
    for (const auto& [stage, rows] : stages_) {
        for (const IterMetrics& m : rows) {
            out << stage << ',' << m.iter << ',' << m.hpwl << ',' << m.overflow << ','
                << m.lambda << ',' << m.gamma << ',' << m.step_size << ','
                << m.grad_norm_wl << ',' << m.grad_norm_den << ',' << m.max_displacement
                << ',' << m.elapsed_ms << '\n';
        }
    }
}

void MetricsSink::writeSummary(const std::string& path) const {
    std::ofstream out(path);
    if (!out) {
        SP_WARN("cannot write %s", path.c_str());
        return;
    }
    out << std::setprecision(10);
    out << "{\n";

    bool first = true;
    auto comma = [&]() {
        if (!first) out << ",\n";
        first = false;
    };

    for (const auto& [k, v] : summary_str_) {
        comma();
        out << "  \"" << k << "\": \"" << v << "\"";
    }
    for (const auto& [k, v] : summary_num_) {
        comma();
        out << "  \"" << k << "\": " << v;
    }
    if (!durations_.empty()) {
        comma();
        out << "  \"durations_ms\": {\n";
        bool f2 = true;
        for (const auto& [k, v] : durations_) {
            if (!f2) out << ",\n";
            f2 = false;
            out << "    \"" << k << "\": " << v;
        }
        out << "\n  }";
    }
    out << "\n}\n";
}

// ------------------------------------------------------------------ ScopedTimer
ScopedTimer::ScopedTimer(MetricsSink& sink, std::string label)
    : sink_(sink), label_(std::move(label)) {}

ScopedTimer::~ScopedTimer() { sink_.recordDuration(label_, timer_.elapsedMs()); }

}  // namespace sp
