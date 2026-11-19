// Isolate the Poisson solver; planning and warm-up are excluded from solve_ms.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <numeric>
#include <stdexcept>
#include <vector>
#include <omp.h>
#include "numeric/poisson_backend.h"

int main(int argc,char** argv) {
    try {
        const int n=argc>1?std::stoi(argv[1]):512;
        const int workers=argc>2?std::stoi(argv[2]):4;
        const int rounds=argc>3?std::stoi(argv[3]):100;
        const int ompThreads=argc>4?std::stoi(argv[4]):1;
        if(n<2 || rounds<1 || ompThreads<1) throw std::runtime_error("invalid benchmark dimensions/iterations/threads");
        omp_set_dynamic(0); omp_set_num_threads(ompThreads);
        std::vector<float> rho(size_t(n)*n),phi(rho.size()),ex(rho.size()),ey(rho.size());
        for(size_t i=0;i<rho.size();++i) rho[i]=std::sin(float(i)*.013f)+std::cos(float(i)*.037f)+float(i%97)/97.f;
        const auto start=std::chrono::steady_clock::now();
        auto backend=sp::makePoissonBackend({n,n,1.25f,.75f,false,workers});
        const double planMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        auto solve=[&] { backend->solve({rho.data(),n,n},{phi.data(),n,n},{ex.data(),n,n},{ey.data(),n,n}); };
        for(int k=0;k<10;++k) solve();
        const auto timed=std::chrono::steady_clock::now();
        for(int k=0;k<rounds;++k) solve();
        const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-timed).count()/rounds;
        std::printf("{\"grid\":%d,\"fftw_threads\":%d,\"fftw_plan_threads\":%d,\"omp_threads\":%d,\"iterations\":%d,\"plan_ms\":%.6f,\"solve_ms\":%.6f,\"checksum\":%.9g}\n",n,workers,backend->cpuThreads(),ompThreads,rounds,planMs,ms,std::accumulate(phi.begin(),phi.end(),0.0));
    } catch(const std::exception& e) { std::fprintf(stderr,"%s\n",e.what()); return 1; }
}
