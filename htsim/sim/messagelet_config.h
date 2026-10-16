#ifndef MESSAGELET_CONFIG_H
#define MESSAGELET_CONFIG_H

#include <cstdint>
#include <string>

struct MessageletConfig {
    static constexpr uint32_t kSchemaVersion = 1;

    enum class Granularity { Flow, Messagelet, Packet };

    struct RuntimeSettings {
        Granularity granularity = Granularity::Flow;
    } runtime;

    struct MessageletSettings {
        // Positive only in messagelet mode. Legacy configs infer flow mode
        // from zero; explicit packet mode uses the transport packet size.
        uint64_t size_bytes = 0;
    } messagelet;

    struct RoutingSettings {
        std::string default_route = "minimum";
        std::string network_decision = "none";
    } routing;

    struct LoggingSettings {
        bool trace_decisions = false;
    } logging;

    static MessageletConfig loadFromFile(const std::string& filename);
    static const char* granularityName(Granularity granularity);
    void validate() const;
};

#endif // MESSAGELET_CONFIG_H
