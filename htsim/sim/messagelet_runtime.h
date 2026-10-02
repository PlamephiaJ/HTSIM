#ifndef MESSAGELET_RUNTIME_H
#define MESSAGELET_RUNTIME_H

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "messagelet_config.h"

class AtlahsDragonflyApi;
class EventOver;
class SendEvent;
class UecSrc;
class graph_node_properties;

// Boundary between the GOAL scheduler and Dragonfly's HTSim transport.
//
// One GOAL send owns one UecSrc and one FlowState. UecSrc groups new packets
// into messagelets and consults this runtime only at messagelet boundaries;
// completion events are forwarded to GOAL unchanged.
class MessageletRuntime {
public:
    struct RouteInfo {
        uint32_t path_id = 0;
        std::string description = "minimum";
        std::map<std::string, std::string> log_info;
    };

    struct FlowState {
        uint32_t from = 0;
        uint32_t to = 0;
        uint32_t tag = 0;
        uint64_t flow_size = 0;
        uint64_t messagelet_size = 0;
        RouteInfo default_route;
        std::vector<RouteInfo> messagelet_routes;
    };

    explicit MessageletRuntime(AtlahsDragonflyApi& goal_runtime);

    void submit(const SendEvent& event, graph_node_properties node);
    void eventFinished(const EventOver& event);

    void loadConfig(const std::string& filename);
    const MessageletConfig& config() const { return _config; }

    uint64_t messageletSize(const UecSrc& source) const;
    uint32_t pathIdForMessagelet(const UecSrc& source, uint64_t messagelet_index);

    // Per-flow policy and inspection hooks for later routing decisions.
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
                      uint32_t tag);
    RouteInfo minimumRoute(uint32_t from, uint32_t to) const;
    FlowState& mutableFlowState(const UecSrc& source);

    AtlahsDragonflyApi& _goal_runtime;
    MessageletConfig _config;
    std::unordered_map<const UecSrc*, FlowState> _flows;
};

#endif // MESSAGELET_RUNTIME_H
