#include "atlahs_dragonfly_api.h"

#include "messagelet_runtime.h"
#include "uec_mp.h"

AtlahsDragonflyApi::AtlahsDragonflyApi()
    : _messagelet_runtime(std::make_unique<MessageletRuntime>(*this)) {}

AtlahsDragonflyApi::~AtlahsDragonflyApi() = default;

void AtlahsDragonflyApi::Send(const SendEvent& event, graph_node_properties elem) {
    _messagelet_runtime->submit(event, elem);
}
