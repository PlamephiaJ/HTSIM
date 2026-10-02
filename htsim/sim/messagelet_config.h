#ifndef MESSAGELET_CONFIG_H
#define MESSAGELET_CONFIG_H

#include <cstdint>
#include <string>

struct MessageletConfig {
    static constexpr uint32_t kSchemaVersion = 1;

    struct MessageletSettings {
        // Zero means that each parent flow is a single messagelet.
        uint64_t size_bytes = 0;
    } messagelet;

    struct RoutingSettings {
        std::string default_route = "minimum";
    } routing;

    struct LoggingSettings {
        bool trace_decisions = false;
    } logging;

    static MessageletConfig loadFromFile(const std::string& filename);
    void validate() const;
};

#endif // MESSAGELET_CONFIG_H
