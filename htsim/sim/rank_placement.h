#ifndef RANK_PLACEMENT_H
#define RANK_PLACEMENT_H

#include <cstdint>
#include <string>
#include <vector>

// Maps effective GOAL network ranks to explicit Dragonfly endpoints. For
// current GOAL workloads (one NIC per rank), the effective rank is the GOAL
// rank itself.
class RankPlacement {
public:
    static constexpr uint32_t kSchemaVersion = 1;

    static RankPlacement loadFromFile(const std::string& filename);

    void resolveAndValidate(uint32_t rank_count,
                            uint32_t hosts_per_switch,
                            uint32_t switches_per_group,
                            uint32_t group_count);
    uint32_t hostForRank(uint32_t rank) const;

private:
    struct Endpoint {
        uint32_t rank;
        uint32_t group;
        uint32_t switch_in_group;
        uint32_t host_in_switch;
    };

    std::vector<Endpoint> _endpoints;
    std::vector<uint32_t> _rank_to_host;
};

#endif // RANK_PLACEMENT_H
