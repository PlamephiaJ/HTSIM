#ifndef ATLAHS_DRAGONFLY_API_H
#define ATLAHS_DRAGONFLY_API_H

#include <memory>

#include "atlahs_htsim_api.h"

class DragonflyTopology;
class MessageletRuntime;

class AtlahsDragonflyApi : public AtlahsHtsimApi {
public:
    AtlahsDragonflyApi();
    ~AtlahsDragonflyApi() override;

    void Send(const SendEvent& event, graph_node_properties node) override;

    void setDragonflyTopology(DragonflyTopology* topo) { _df_topo = topo; }
    DragonflyTopology* getDragonflyTopology() const { return _df_topo; }

    void setMaxRtt(simtime_picosec max_rtt) { _max_rtt = max_rtt; }
    simtime_picosec getMaxRtt() const { return _max_rtt; }

    MessageletRuntime& getMessageletRuntime() { return *_messagelet_runtime; }

private:
    friend class MessageletRuntime;

    DragonflyTopology* _df_topo = nullptr;
    simtime_picosec _max_rtt = 0;
    std::unique_ptr<MessageletRuntime> _messagelet_runtime;
};

#endif // ATLAHS_DRAGONFLY_API_H
