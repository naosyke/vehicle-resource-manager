// DDS plumbing shared by the manager and managed nodes: topic names, QoS,
// and conversions between the IDL enums and vrm::State / vrm::Transition.
#pragma once

#include <dds/dds.hpp>

#include "LifecycleMsgs.hpp"
#include "vrm/lifecycle.hpp"

namespace vrm::dds_lifecycle {

inline constexpr const char* kCommandTopic = "vrm_lifecycle_command";
inline constexpr const char* kStatusTopic = "vrm_lifecycle_status";

// Environment variable through which the manager passes its id to nodes.
inline constexpr const char* kManagerIdEnv = "VRM_MANAGER_ID";

State from_msg(msg::LifecycleState state);
msg::LifecycleState to_msg(State state);
Transition from_msg(msg::LifecycleTransition transition);
msg::LifecycleTransition to_msg(Transition transition);

// Commands must not be lost, but are meaningless to a node started later.
dds::pub::qos::DataWriterQos command_writer_qos(const dds::pub::Publisher& publisher);
dds::sub::qos::DataReaderQos command_reader_qos(const dds::sub::Subscriber& subscriber);

// Status keeps the latest sample per node for late joiners.
dds::pub::qos::DataWriterQos status_writer_qos(const dds::pub::Publisher& publisher);
dds::sub::qos::DataReaderQos status_reader_qos(const dds::sub::Subscriber& subscriber);

}  // namespace vrm::dds_lifecycle
