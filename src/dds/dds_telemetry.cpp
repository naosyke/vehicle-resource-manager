#include "vrm/dds_telemetry.hpp"

namespace vrm::dds_telemetry {

using dds::core::policy::Durability;
using dds::core::policy::History;
using dds::core::policy::Reliability;

dds::pub::qos::DataWriterQos status_writer_qos(const dds::pub::Publisher& publisher) {
    auto qos = publisher.default_datawriter_qos();
    qos << Reliability::Reliable() << Durability::TransientLocal() << History::KeepLast(1);
    return qos;
}

dds::sub::qos::DataReaderQos status_reader_qos(const dds::sub::Subscriber& subscriber) {
    auto qos = subscriber.default_datareader_qos();
    qos << Reliability::Reliable() << Durability::TransientLocal() << History::KeepLast(1);
    return qos;
}

dds::topic::qos::TopicQos event_topic_qos(const dds::domain::DomainParticipant& participant) {
    auto qos = participant.default_topic_qos();
    // How much history late joiners get is the durability service history
    // (default: keep last 1), not the writer's History policy.
    qos << Reliability::Reliable() << Durability::TransientLocal() << History::KeepLast(kEventHistory)
        << dds::core::policy::DurabilityService(dds::core::Duration::zero(),
                                                dds::core::policy::HistoryKind::KEEP_LAST, kEventHistory,
                                                dds::core::LENGTH_UNLIMITED, dds::core::LENGTH_UNLIMITED,
                                                dds::core::LENGTH_UNLIMITED);
    return qos;
}

dds::pub::qos::DataWriterQos event_writer_qos(const dds::topic::Topic<msg::SystemEvent>& topic) {
    dds::pub::qos::DataWriterQos qos;
    qos = topic.qos();  // Takes reliability, durability, history and durability service.
    return qos;
}

dds::sub::qos::DataReaderQos event_reader_qos(const dds::sub::Subscriber& subscriber) {
    auto qos = subscriber.default_datareader_qos();
    qos << Reliability::Reliable() << Durability::TransientLocal() << History::KeepLast(kEventHistory);
    return qos;
}

}  // namespace vrm::dds_telemetry
