#ifndef GROUP_ROUTING_AGENT_H
#define GROUP_ROUTING_AGENT_H

#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>
#include "messagelet.h"
#include "gra_static_graph.h"
#include "gra_execution_progress.h"
#include "routing_demand_policy.h"
#include "routing_priority.h"

class DragonflyTopology;
class Packet;
class Queue;

// Ground truth state for one group. A corridor is a directed global egress
// queue plus its link. Q uses wire bytes; R uses original payload bytes.
// Queries have no simulated cost. No dissemination/synchronization is modeled.
class GroupRoutingAgent {
public:
    using Corridor = std::pair<uint32_t, uint32_t>; // egress switch, remote switch
    using Messagelet = MessageletKey;
    struct CorridorState { Corridor corridor; uint64_t q_bytes; uint64_t r_bytes; };
    struct Commitment {
        Corridor corridor;
        uint64_t original_bytes;
        uint64_t remaining_bytes;
        uint64_t transport_flow_id;
    };
    struct Snapshot {
        uint64_t version = 0;
        std::vector<CorridorState> corridors;
        std::map<Messagelet, Commitment> messagelets; // pending only; omitted => R_m,c = 0
    };

    GroupRoutingAgent(DragonflyTopology& topology, uint32_t group);
    uint32_t group() const { return _group; }
    // Installed once before GOAL scheduling; every group shares the same
    // immutable global result. Keys deliberately omit messagelet_idx.
    const GraStaticGraph& staticGraph() const;
    const GraExecutionProgress& executionProgress() const;
    GraExecutionProgress::Result receiveProgress(const GraProgressReport& report);
    std::vector<GraStaticGraph::EventKey> frontier(uint64_t dag_index) const {
        return executionProgress().frontier(dag_index);
    }
    // Resolve SOURCE's two-hop label (including explicit waypoint labels).
    std::optional<Corridor> corridorForRoute(uint32_t from, uint32_t to, uint32_t path_id) const;
    CorridorState query(Corridor corridor) const;
    uint64_t remaining(Messagelet messagelet) const;
    Snapshot snapshot() const;

    // Optional pre-routing strategy. Disabled GRAs allocate no demand state.
    // Install before READY reports. A replacement policy owns its own state.
    void setDemandPolicy(std::unique_ptr<RoutingDemandPolicy> policy, double alpha = 1.0);
    void setPriorityStrategy(std::shared_ptr<const RoutingPriorityStrategy> strategy);
    const RoutingDemandPolicy* demandPolicy() const { return _demand_policy.get(); }
    void messageletReady(Messagelet messagelet, uint32_t from, uint32_t to, uint64_t payload_bytes);
    void cancelReady(Messagelet messagelet);
    void routeDecided(Messagelet messagelet);
    void refreshDemand(const char* reason = "manual", std::optional<Messagelet> trigger = std::nullopt);
    uint64_t softClaimPressure(Messagelet requester, Corridor corridor) const;
    RoutingDemandPolicy::PressureObservation observeSoftClaim(std::optional<Messagelet> requester,
                                                             Corridor corridor) const;
    uint64_t demandSnapshotId() const { return _demand_snapshot_id; }

    void commit(Messagelet messagelet, Corridor corridor, uint64_t payload_bytes,
                std::optional<uint64_t> transport_flow_id = std::nullopt);
    void bindPacket(Messagelet messagelet, uint64_t seqno, uint64_t payload_bytes);
    // Entry switches can see the same original packet again after a drop.
    void bindPacketOnce(Messagelet messagelet, uint64_t seqno, uint64_t payload_bytes);
    void admitted(Corridor corridor, const Packet& packet);
    void setTrace(bool enabled) { _trace = enabled; }

private:
    friend class DragonflyTopology;
    std::shared_ptr<const GraStaticGraph> _static_graph;
    std::unique_ptr<GraExecutionProgress> _execution_progress;
    void log(const char* action, Messagelet messagelet, const Commitment& state,
             uint64_t bytes, std::optional<uint64_t> seqno = std::nullopt) const;
    DragonflyTopology& _topology;
    uint32_t _group;
    bool _trace = false;
    uint64_t _version = 0;
    std::map<Corridor, Queue*> _queues;
    std::map<Corridor, uint64_t> _remaining;
    std::map<Messagelet, Commitment> _commitments;
    std::unique_ptr<RoutingDemandPolicy> _demand_policy;
    double _demand_alpha = 1.0;
    uint64_t _demand_snapshot_id = 0; // trace-only, independent of Q/R version
    std::shared_ptr<const RoutingPriorityStrategy> _priority_strategy;
    RoutingPriorityStrategy::Estimates _priority_estimates;
    void tracePreferences(const char* reason, std::optional<Messagelet> trigger,
                          const std::map<Messagelet, std::optional<Corridor>>& preferences);
    void removeDemand(Messagelet messagelet, const char* action);
    void traceDemand(const char* action, Messagelet messagelet) const;
    struct PacketState { Messagelet messagelet; uint64_t bytes; bool entered = false; };
    std::map<std::pair<uint64_t, uint64_t>, PacketState> _packets; // flow, seqno
};

#endif
