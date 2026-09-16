#include "init/initial_placer.h"

#include "init/cluster_placer.h"
#include "init/quadratic_placer.h"
#include "init/random_placer.h"

namespace sp {

const std::vector<std::string>& initialPlacerNames() {
    static const std::vector<std::string> kNames = {"random", "cluster_fc", "cluster_bc",
                                                    "quadratic"};
    return kNames;
}

std::unique_ptr<IInitialPlacer> makeInitialPlacer(const std::string& method) {
    if (method == "random") return std::make_unique<RandomPlacer>();
    if (method == "cluster_fc") return std::make_unique<ClusterPlacer>(MergeStrategy::kFirstChoice);
    if (method == "cluster_bc") return std::make_unique<ClusterPlacer>(MergeStrategy::kBestChoice);
    if (method == "quadratic") return std::make_unique<QuadraticPlacer>();
    return nullptr;
}

}  // namespace sp
