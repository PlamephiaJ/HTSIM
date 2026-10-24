#include "atlahs_dragonfly_api.h"

#include "messagelet_runtime.h"
#include "uec_mp.h"

AtlahsDragonflyApi::AtlahsDragonflyApi()
    : _messagelet_runtime(std::make_unique<MessageletRuntime>(*this)) {}

AtlahsDragonflyApi::~AtlahsDragonflyApi() = default;

void AtlahsDragonflyApi::Send(const SendEvent& event, graph_node_properties elem) {
    _messagelet_runtime->submit(event, elem);
}

void AtlahsDragonflyApi::initializeGoalGraph(const Parser& parser) {
    _messagelet_runtime->initializeGoalGraph(parser);
}

void AtlahsDragonflyApi::reportTaskProgress(uint32_t rank, uint32_t task_offset, bool finished,
                                           uint64_t event_time_ns, uint8_t nic) {
    _messagelet_runtime->reportTaskProgress(rank, task_offset, finished, event_time_ns, nic);
}
