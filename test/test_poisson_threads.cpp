#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <vector>

#include <omp.h>
#include "numeric/poisson_backend.h"
#include "util/config.h"
#include "test_util.h"

namespace {
struct Field {
    int nx, ny;
    std::vector<float> rho, phi, ex, ey;
    Field(int x, int y) : nx(x), ny(y), rho(x*y), phi(x*y), ex(x*y), ey(x*y) {
        for (int i=0; i<x*y; ++i)
            rho[i] = std::sin(i*.013f) + std::cos(i*.037f) + float(i%97)/97.f;
    }
    sp::BinGridSpan span(std::vector<float>& a) { return {a.data(),nx,ny}; }
    void solve(sp::PoissonBackend& be) { be.solve(span(rho),span(phi),span(ex),span(ey)); }
};

auto backend(int x,int y,int threads) {
    return sp::makePoissonBackend({x,y,1.25f,.75f,false,threads});
}
void compare(const std::vector<float>& a,const std::vector<float>& b) {
    float scale=0, error=0;
    for (size_t i=0;i<a.size();++i) {
        CHECK_TRUE(std::isfinite(b[i]));
        scale=std::max(scale,std::fabs(a[i]));
        error=std::max(error,std::fabs(a[i]-b[i]));
    }
    CHECK_TRUE(error <= 5e-5f*std::max(1.f,scale));
}
void numericAndDeterminism() {
    for (auto dims : {std::pair{17,29},std::pair{512,128},std::pair{512,512}}) {
        Field reference(dims.first,dims.second);
        omp_set_num_threads(1);
        auto serial=backend(dims.first,dims.second,1); reference.solve(*serial);
        Field first(dims.first,dims.second);
        for (int ompThreads : {1,8,32}) {
            omp_set_num_threads(ompThreads);
            Field value(dims.first,dims.second); const auto rho=value.rho;
            auto threaded=backend(dims.first,dims.second,4); value.solve(*threaded);
            CHECK_EQ(threaded->cpuThreads(), dims.first*dims.second>=256*256 ? 4 : 1);
            CHECK_TRUE(value.rho==rho);
            compare(reference.phi,value.phi); compare(reference.ex,value.ex); compare(reference.ey,value.ey);
            if (ompThreads==1) first=value;
            else {
                CHECK_TRUE(first.phi==value.phi);
                CHECK_TRUE(first.ex==value.ex); CHECK_TRUE(first.ey==value.ey);
            }
            value.solve(*threaded);
            CHECK_TRUE(first.phi==value.phi);
        }
        // Destroying another backend must not clean up process-global FFTW state.
        { auto temporary=backend(dims.first,dims.second,2); }
        Field again(dims.first,dims.second); again.solve(*serial);
        CHECK_TRUE(reference.phi==again.phi);
    }
    omp_set_num_threads(1);
    std::vector<std::future<Field>> tasks;
    for (int i=0;i<4;++i) tasks.push_back(std::async(std::launch::async,[] {
        omp_set_num_threads(1);
        Field f(256,256); auto be=backend(256,256,4); f.solve(*be); return f;
    }));
    const auto expected=tasks.front().get();
    for (size_t i=1;i<tasks.size();++i) {
        const auto f=tasks[i].get(); CHECK_TRUE(f.phi==expected.phi);
        CHECK_TRUE(f.ex==expected.ex); CHECK_TRUE(f.ey==expected.ey);
    }
    bool rejected=false;
    try { auto bad=backend(8,8,0); } catch (const std::runtime_error&) { rejected=true; }
    CHECK_TRUE(rejected);
}
void configContract() {
    const auto p=std::filesystem::temp_directory_path()/
        ("fftw-config-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".json");
    for (const char* value : {"0","-1","1.5","true","4294967297","-4294967295"}) {
        { std::ofstream f(p); f << "{\"fftw_threads\":" << value << "}"; }
        bool rejected=false;
        try { sp::Config c; c.loadJson(p.string()); } catch (const std::exception&) { rejected=true; }
        CHECK_TRUE(rejected);
    }
    { std::ofstream f(p); f << "{\"fftw_threads\":1}"; }
    std::vector<std::string> args={"test","--aux","fixture.aux","--config",p.string(),
                                   "--threads","32","--fftw-threads","4"};
    std::vector<char*> argv; for (auto& a:args) argv.push_back(a.data());
    const auto c=sp::Config::fromArgs(int(argv.size()),argv.data());
    CHECK_EQ(c.fftw_threads,4); CHECK_EQ(c.num_threads,32);
    std::filesystem::remove(p);
}
}
int main() {
    omp_set_dynamic(0);
    numericAndDeterminism(); configContract();
    return sptest::summary("test_poisson_threads");
}
