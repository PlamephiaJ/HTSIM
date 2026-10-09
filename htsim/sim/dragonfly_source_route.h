#ifndef DRAGONFLY_SOURCE_ROUTE_H
#define DRAGONFLY_SOURCE_ROUTE_H

#include <cstdint>

namespace DragonflySourceRoute {
// Legacy SOURCE paths encode two 16-bit switch IDs. Manual valiant paths
// reserve the top bit and encode one waypoint switch in the remaining bits.
// A packet travels minimally to the waypoint, then minimally to its destination.
// The packet's waypoint-completed flag prevents a loop on same-group sends.
constexpr uint32_t kWaypointFlag = uint32_t{1} << 31;
constexpr uint32_t kWaypointMask = kWaypointFlag - 1;
} // namespace DragonflySourceRoute

#endif
