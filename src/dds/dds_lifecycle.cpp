#include "vrm/dds_lifecycle.hpp"

namespace vrm::dds_lifecycle {

using dds::core::policy::Durability;
using dds::core::policy::History;
using dds::core::policy::Reliability;

State from_msg(msg::LifecycleState state) {
    switch (state) {
        case msg::LifecycleState::STATE_UNCONFIGURED: return State::Unconfigured;
        case msg::LifecycleState::STATE_INACTIVE: return State::Inactive;
        case msg::LifecycleState::STATE_ACTIVE: return State::Active;
        case msg::LifecycleState::STATE_FINALIZED: return State::Finalized;
        case msg::LifecycleState::STATE_CONFIGURING: return State::Configuring;
        case msg::LifecycleState::STATE_CLEANING_UP: return State::CleaningUp;
        case msg::LifecycleState::STATE_ACTIVATING: return State::Activating;
        case msg::LifecycleState::STATE_DEACTIVATING: return State::Deactivating;
        case msg::LifecycleState::STATE_SHUTTING_DOWN: return State::ShuttingDown;
        case msg::LifecycleState::STATE_ERROR_PROCESSING: return State::ErrorProcessing;
    }
    return State::Unconfigured;
}

msg::LifecycleState to_msg(State state) {
    switch (state) {
        case State::Unconfigured: return msg::LifecycleState::STATE_UNCONFIGURED;
        case State::Inactive: return msg::LifecycleState::STATE_INACTIVE;
        case State::Active: return msg::LifecycleState::STATE_ACTIVE;
        case State::Finalized: return msg::LifecycleState::STATE_FINALIZED;
        case State::Configuring: return msg::LifecycleState::STATE_CONFIGURING;
        case State::CleaningUp: return msg::LifecycleState::STATE_CLEANING_UP;
        case State::Activating: return msg::LifecycleState::STATE_ACTIVATING;
        case State::Deactivating: return msg::LifecycleState::STATE_DEACTIVATING;
        case State::ShuttingDown: return msg::LifecycleState::STATE_SHUTTING_DOWN;
        case State::ErrorProcessing: return msg::LifecycleState::STATE_ERROR_PROCESSING;
    }
    return msg::LifecycleState::STATE_UNCONFIGURED;
}

Transition from_msg(msg::LifecycleTransition transition) {
    switch (transition) {
        case msg::LifecycleTransition::TRANSITION_CONFIGURE: return Transition::Configure;
        case msg::LifecycleTransition::TRANSITION_CLEANUP: return Transition::Cleanup;
        case msg::LifecycleTransition::TRANSITION_ACTIVATE: return Transition::Activate;
        case msg::LifecycleTransition::TRANSITION_DEACTIVATE: return Transition::Deactivate;
        case msg::LifecycleTransition::TRANSITION_SHUTDOWN: return Transition::Shutdown;
    }
    return Transition::Shutdown;
}

msg::LifecycleTransition to_msg(Transition transition) {
    switch (transition) {
        case Transition::Configure: return msg::LifecycleTransition::TRANSITION_CONFIGURE;
        case Transition::Cleanup: return msg::LifecycleTransition::TRANSITION_CLEANUP;
        case Transition::Activate: return msg::LifecycleTransition::TRANSITION_ACTIVATE;
        case Transition::Deactivate: return msg::LifecycleTransition::TRANSITION_DEACTIVATE;
        case Transition::Shutdown: return msg::LifecycleTransition::TRANSITION_SHUTDOWN;
    }
    return msg::LifecycleTransition::TRANSITION_SHUTDOWN;
}

dds::pub::qos::DataWriterQos command_writer_qos(const dds::pub::Publisher& publisher) {
    auto qos = publisher.default_datawriter_qos();
    qos << Reliability::Reliable() << Durability::Volatile() << History::KeepLast(16);
    return qos;
}

dds::sub::qos::DataReaderQos command_reader_qos(const dds::sub::Subscriber& subscriber) {
    auto qos = subscriber.default_datareader_qos();
    qos << Reliability::Reliable() << Durability::Volatile() << History::KeepLast(16);
    return qos;
}

dds::pub::qos::DataWriterQos status_writer_qos(const dds::pub::Publisher& publisher) {
    auto qos = publisher.default_datawriter_qos();
    qos << Reliability::Reliable() << Durability::TransientLocal() << History::KeepLast(1);
    return qos;
}

dds::sub::qos::DataReaderQos status_reader_qos(const dds::sub::Subscriber& subscriber) {
    auto qos = subscriber.default_datareader_qos();
    // Keep a few samples per node so transition states are not overwritten
    // before the manager takes them.
    qos << Reliability::Reliable() << Durability::TransientLocal() << History::KeepLast(8);
    return qos;
}

dds::pub::qos::DataWriterQos heartbeat_writer_qos(const dds::pub::Publisher& publisher) {
    auto qos = publisher.default_datawriter_qos();
    qos << Reliability::BestEffort() << Durability::Volatile() << History::KeepLast(1);
    return qos;
}

dds::sub::qos::DataReaderQos heartbeat_reader_qos(const dds::sub::Subscriber& subscriber) {
    auto qos = subscriber.default_datareader_qos();
    qos << Reliability::BestEffort() << Durability::Volatile() << History::KeepLast(1);
    return qos;
}

}  // namespace vrm::dds_lifecycle
