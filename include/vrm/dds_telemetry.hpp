// DDS topics and QoS for the manager's telemetry (see idl/TelemetryMsgs.idl).
#pragma once

#include <dds/dds.hpp>

#include "TelemetryMsgs.hpp"

namespace vrm::dds_telemetry {

inline constexpr const char* kNodeStatusTopic = "vrm_node_status";
inline constexpr const char* kSystemEventTopic = "vrm_system_event";
inline constexpr int kEventHistory = 100;

// Latest status per node, also for late joiners.
dds::pub::qos::DataWriterQos status_writer_qos(const dds::pub::Publisher& publisher);
dds::sub::qos::DataReaderQos status_reader_qos(const dds::sub::Subscriber& subscriber);

// Recent events per manager, also for late joiners. The history for late
// joiners is a topic-level policy (durability service), so the writer takes
// its QoS from the topic.
dds::topic::qos::TopicQos event_topic_qos(const dds::domain::DomainParticipant& participant);
dds::pub::qos::DataWriterQos event_writer_qos(const dds::topic::Topic<msg::SystemEvent>& topic);
dds::sub::qos::DataReaderQos event_reader_qos(const dds::sub::Subscriber& subscriber);

}  // namespace vrm::dds_telemetry
