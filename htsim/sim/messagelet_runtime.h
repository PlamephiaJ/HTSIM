#ifndef MESSAGELET_RUNTIME_H
#define MESSAGELET_RUNTIME_H

class AtlahsDragonflyApi;
class EventOver;
class SendEvent;
class graph_node_properties;

// Boundary between the GOAL scheduler and Dragonfly's HTSim transport.
//
// The initial implementation is deliberately transparent: one GOAL send is
// submitted as one UecSrc flow, and the resulting EventOver is forwarded
// unchanged.  Messagelet splitting, path selection, and per-messagelet state
// can be added here without exposing those concerns to either side.
class MessageletRuntime {
public:
    explicit MessageletRuntime(AtlahsDragonflyApi& goal_runtime);

    void submit(const SendEvent& event, graph_node_properties node);
    void eventFinished(const EventOver& event);

private:
    AtlahsDragonflyApi& _goal_runtime;
};

#endif // MESSAGELET_RUNTIME_H
