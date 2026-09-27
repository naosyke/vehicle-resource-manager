// Publishes the manager's view of the system over DDS (see TelemetryMsgs.idl).
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "TelemetryMsgs.hpp"

namespace vrm {

class TelemetryPublisher {
public:
    explicit TelemetryPublisher(std::uint64_t manager_id);
    ~TelemetryPublisher();

    void publish_status(const msg::NodeStatusReport& report);
    void publish_event(double time, const std::string& level, const std::string& node, const std::string& message);

private:
    struct Dds;
    std::unique_ptr<Dds> dds_;
    std::uint64_t manager_id_;
    std::uint64_t next_event_id_ = 1;
};

}  // namespace vrm
