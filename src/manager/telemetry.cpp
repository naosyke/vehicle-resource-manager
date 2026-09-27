#include "telemetry.hpp"

#include "vrm/dds_telemetry.hpp"

namespace vrm {

struct TelemetryPublisher::Dds {
    dds::domain::DomainParticipant participant{org::eclipse::cyclonedds::domain::default_id()};
    dds::pub::Publisher publisher{participant};
    dds::topic::Topic<msg::NodeStatusReport> status_topic{participant, dds_telemetry::kNodeStatusTopic};
    dds::topic::Topic<msg::SystemEvent> event_topic{participant, dds_telemetry::kSystemEventTopic,
                                                    dds_telemetry::event_topic_qos(participant)};
    dds::pub::DataWriter<msg::NodeStatusReport> status_writer{publisher, status_topic,
                                                              dds_telemetry::status_writer_qos(publisher)};
    dds::pub::DataWriter<msg::SystemEvent> event_writer{publisher, event_topic,
                                                        dds_telemetry::event_writer_qos(event_topic)};
};

TelemetryPublisher::TelemetryPublisher(std::uint64_t manager_id)
    : dds_(std::make_unique<Dds>()), manager_id_(manager_id) {}

TelemetryPublisher::~TelemetryPublisher() = default;

void TelemetryPublisher::publish_status(const msg::NodeStatusReport& report) {
    dds_->status_writer.write(report);
}

void TelemetryPublisher::publish_event(double time, const std::string& level, const std::string& node,
                                       const std::string& message) {
    dds_->event_writer.write(msg::SystemEvent(manager_id_, next_event_id_++, time, level, node, message));
}

}  // namespace vrm
