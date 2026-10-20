#ifndef MESSAGELET_RUNTIME_H
#define MESSAGELET_RUNTIME_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "messagelet_config.h"
#include "flow_routing_config.h"
#include "network_decision.h"

class AtlahsDragonflyApi;
class EventOver;
class SendEvent;
class UecSrc;
class graph_node_properties;

// Boundary between the GOAL scheduler and Dragonfly's HTSim transport.
//
// One GOAL send owns one UecSrc and one FlowState. In label mode, UecSrc
// consults this runtime at messagelet boundaries. Transparent mode uses the
// original UEC routing hooks. Both preserve GOAL completion handling.
class MessageletRuntime {
public:
    using RouteInfo = NetworkRoute;
    using Granularity = MessageletConfig::Granularity;

    struct FlowState {
        uint64_t flow_id = 0;
        bool has_flow_id = false;
        uint32_t from = 0;
        uint32_t to = 0;
        uint32_t tag = 0;
        uint64_t flow_size = 0;
        uint64_t original_payload_sent = 0;
        uint64_t messagelet_size = 0;
        Granularity granularity = Granularity::Flow;
        RouteInfo default_route;
        std::vector<RouteInfo> messagelet_routes;
        std::unordered_set<uint64_t> finalized_routes;
        std::unordered_set<uint64_t> route_overrides;
    };

    explicit MessageletRuntime(AtlahsDragonflyApi& goal_runtime);

    void submit(const SendEvent& event, graph_node_properties node);
    void eventFinished(const EventOver& event);

    void loadConfig(const std::string& filename);
    void loadRoutingConfig(const std::string& filename);
    void validateRoutingComplete() const;
    const MessageletConfig& config() const { return _config; }
    bool transparent() const { return _config.runtime.mode == MessageletConfig::Mode::Transparent; }

    uint64_t messageletSize(const UecSrc& source) const;
    Granularity granularity(const UecSrc& source) const;
    uint32_t pathIdForMessagelet(const UecSrc& source, uint64_t messagelet_index);

    void bindPacket(const UecSrc& source, uint64_t messagelet_index, uint64_t seqno, uint64_t payload_bytes);

    // Replacement affects only messagelets that have not yet been labelled.
    // A null policy restores static/default routing for future messagelets.
    void setNetworkDecision(std::unique_ptr<NetworkDecision> decision);

    // Per-flow policy and inspection hooks for later routing decisions.
    // Changes apply when the next messagelet starts; active labels stay fixed.
    void setGranularity(const UecSrc& source, Granularity granularity, uint64_t size_bytes = 0);
    void setMessageletSize(const UecSrc& source, uint64_t size);
    void setMessageletRoute(const UecSrc& source,
                            uint64_t messagelet_index,
                            RouteInfo route_info);
    const FlowState& flowState(const UecSrc& source) const;

private:
    void registerFlow(UecSrc& source,
                      uint64_t flow_size,
                      uint32_t from,
                      uint32_t to,
                      uint32_t tag,
                      const FlowRoutingConfig::Entry* routing);
    RouteInfo minimumRoute(uint32_t from, uint32_t to) const;
    RouteInfo valiantRoute(uint32_t from, uint32_t to, uint64_t flow_id) const;
    FlowState& mutableFlowState(const UecSrc& source);

    AtlahsDragonflyApi& _goal_runtime;
    MessageletConfig _config;
    FlowRoutingConfig _routing_config;
    bool _routing_config_loaded = false;
    std::unique_ptr<NetworkDecision> _network_decision;
    std::unordered_map<const UecSrc*, FlowState> _flows;
    uint64_t _decision_index = 0;
};

#endif // MESSAGELET_RUNTIME_H
