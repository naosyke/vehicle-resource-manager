#include "vrm/managed_node.hpp"

#include <dirent.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>

#include <csignal>
#include <cstdlib>
#include <thread>

#include "vrm/dds_lifecycle.hpp"
#include "vrm/log.hpp"

namespace vrm {

std::atomic<bool> g_stop_requested{false};

namespace {
void on_stop_signal(int) { g_stop_requested = true; }
}  // namespace

void install_stop_signal_handlers() {
    struct sigaction action {};
    action.sa_handler = on_stop_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
}

struct ManagedNode::Dds {
    dds::domain::DomainParticipant participant{org::eclipse::cyclonedds::domain::default_id()};
    dds::topic::Topic<msg::LifecycleCommand> command_topic{participant, dds_lifecycle::kCommandTopic};
    dds::topic::Topic<msg::LifecycleStatus> status_topic{participant, dds_lifecycle::kStatusTopic};
    dds::sub::Subscriber subscriber{participant};
    dds::pub::Publisher publisher{participant};
    dds::sub::DataReader<msg::LifecycleCommand> command_reader{
        subscriber, command_topic, dds_lifecycle::command_reader_qos(subscriber)};
    dds::pub::DataWriter<msg::LifecycleStatus> status_writer{
        publisher, status_topic, dds_lifecycle::status_writer_qos(publisher)};
    dds::topic::Topic<msg::NodeHeartbeat> heartbeat_topic{participant, dds_lifecycle::kHeartbeatTopic};
    dds::pub::DataWriter<msg::NodeHeartbeat> heartbeat_writer{
        publisher, heartbeat_topic, dds_lifecycle::heartbeat_writer_qos(publisher)};
};

ManagedNode::ManagedNode(std::string name, std::chrono::microseconds tick_period,
                         std::chrono::microseconds deadline)
    : name_(std::move(name)),
      tick_period_(tick_period),
      monitor_(deadline.count() > 0 ? deadline : deadline.count() < 0 ? std::chrono::microseconds{0} : tick_period),
      machine_(LifecycleCallbacks{
          [this] { return on_configure(); },
          [this] { return on_cleanup(); },
          [this] { return on_activate(); },
          [this] { return on_deactivate(); },
          [this] { return on_shutdown(); },
          [this] { return on_error(); },
      }),
      dds_(std::make_unique<Dds>()) {
    if (const char* id = std::getenv(dds_lifecycle::kManagerIdEnv)) {
        manager_id_ = std::strtoull(id, nullptr, 10);
    }
    // Report every state change, including transition states.
    machine_.set_observer([this](State, State to) {
        if (!is_primary(to)) {
            publish_status(to, current_request_, true, "");
        }
    });
}

ManagedNode::~ManagedNode() = default;

void ManagedNode::publish_status(State state, std::uint32_t request_id, bool success,
                                 const std::string& message) {
    dds_->status_writer.write(msg::LifecycleStatus(manager_id_, name_, dds_lifecycle::to_msg(state), request_id,
                                                   success, message, static_cast<int32_t>(getpid())));
}

void ManagedNode::handle_command(std::uint32_t request_id, Transition transition) {
    if (request_id != 0 && request_id == last_request_) {
        publish_status(machine_.state(), request_id, last_success_, last_message_);
        return;
    }
    current_request_ = request_id;
    const auto result = machine_.trigger(transition);
    if (result.success) {
        log::info(name_, std::string(to_string(transition)) + " -> " + std::string(to_string(result.state)));
    } else {
        log::warn(name_, std::string(to_string(transition)) + " failed: " + result.message);
    }
    publish_status(result.state, request_id, result.success, result.message);
    current_request_ = 0;
    last_request_ = request_id;
    last_success_ = result.success;
    last_message_ = result.message;
}

void ManagedNode::shutdown_gracefully() {
    if (machine_.state() == State::Active) {
        handle_command(0, Transition::Deactivate);
    }
    if (machine_.state() != State::Finalized) {
        handle_command(0, Transition::Shutdown);
    }
}

void ManagedNode::publish_heartbeat() {
    const int policy = sched_getscheduler(0);
    sched_param param{};
    sched_getparam(0, &param);
    const char* policy_name = policy == SCHED_FIFO ? "SCHED_FIFO"
                              : policy == SCHED_RR ? "SCHED_RR"
                                                   : "SCHED_OTHER";

    const auto window = monitor_.take_window();
    msg::NodeHeartbeat heartbeat;
    heartbeat.manager_id(manager_id_);
    heartbeat.node(name_);
    heartbeat.pid(static_cast<int32_t>(getpid()));
    heartbeat.counter(++heartbeat_sequence_);
    heartbeat.state(dds_lifecycle::to_msg(machine_.state()));
    heartbeat.sched_policy(policy_name);
    heartbeat.sched_priority(param.sched_priority);
    heartbeat.period_us(static_cast<uint32_t>(tick_period_.count()));
    heartbeat.deadline_us(static_cast<uint32_t>(monitor_.deadline().count()));
    heartbeat.total_ticks(monitor_.total_ticks());
    heartbeat.total_misses(monitor_.total_misses());
    heartbeat.window_ticks(static_cast<uint32_t>(window.ticks));
    heartbeat.window_misses(static_cast<uint32_t>(window.misses));
    heartbeat.window_max_latency_us(static_cast<uint32_t>(window.max_latency.count()));
    heartbeat.window_max_response_us(static_cast<uint32_t>(window.max_response.count()));
    heartbeat.window_avg_response_us(static_cast<uint32_t>(window.average_response().count()));
    dds_->heartbeat_writer.write(heartbeat);
}

void ManagedNode::run_tick(std::chrono::steady_clock::time_point& next_tick) {
    const auto release = next_tick;
    const auto start = std::chrono::steady_clock::now();
    on_tick();
    const auto end = std::chrono::steady_clock::now();
    monitor_.record(release, start, end);

    // The next release is run even if late (its latency shows the delay);
    // releases more than a full period behind are skipped and counted as misses.
    next_tick += tick_period_;
    std::uint64_t skipped = 0;
    while (next_tick + tick_period_ <= end) {
        next_tick += tick_period_;
        ++skipped;
    }
    if (skipped) monitor_.record_skipped(skipped);
}

namespace {

// For a SCHED_FIFO node: lock memory so page faults cannot stall a tick, and
// move DDS's helper threads (created with the inherited real-time policy) to
// normal scheduling, so communication never delays the periodic work.
void prepare_realtime(const std::string& name) {
    if (sched_getscheduler(0) != SCHED_FIFO) return;

    // MCL_ONFAULT: lock pages as they are touched. Without it mlockall would
    // populate every mapping up front - including 8 MiB of stack per DDS
    // thread - and put a small node right at its memory limit.
    if (mlockall(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT) != 0) {
        log::warn(name, "mlockall failed; memory is not locked (missing CAP_IPC_LOCK?)");
    }
    const pid_t self = static_cast<pid_t>(syscall(SYS_gettid));
    int demoted = 0;
    if (DIR* tasks = opendir("/proc/self/task")) {
        while (const dirent* entry = readdir(tasks)) {
            const pid_t tid = std::atoi(entry->d_name);
            if (tid <= 0 || tid == self) continue;
            sched_param normal{};
            if (sched_setscheduler(tid, SCHED_OTHER, &normal) == 0) ++demoted;
        }
        closedir(tasks);
    }
    log::info(name, "real-time setup: memory locked, " + std::to_string(demoted) +
                        " DDS threads moved to SCHED_OTHER");
}

}  // namespace

int ManagedNode::run() {
    using Clock = std::chrono::steady_clock;
    prepare_realtime(name_);
    publish_status(machine_.state(), 0, true, "started");
    log::info(name_, "started (pid " + std::to_string(getpid()) + "), waiting for commands");

    auto next_tick = Clock::now();
    auto next_heartbeat = Clock::now();
    bool was_active = false;

    while (machine_.state() != State::Finalized) {
        if (g_stop_requested) {
            log::info(name_, "stop signal received");
            shutdown_gracefully();
            break;
        }

        for (const auto& sample : dds_->command_reader.take()) {
            if (!sample.info().valid() || sample.data().node() != name_) {
                continue;
            }
            // Ignore other systems' managers that happen to use the same node names.
            if (manager_id_ != 0 && sample.data().manager_id() != manager_id_) {
                continue;
            }
            handle_command(sample.data().request_id(), dds_lifecycle::from_msg(sample.data().transition()));
        }

        const bool active = machine_.state() == State::Active;
        if (active && !was_active) {
            next_tick = Clock::now();  // First release right after activation.
            monitor_.reset();
        }
        was_active = active;
        if (active && Clock::now() >= next_tick) {
            run_tick(next_tick);
        }

        const auto now = Clock::now();
        if (now >= next_heartbeat) {
            publish_heartbeat();
            next_heartbeat += dds_lifecycle::kHeartbeatInterval;
            if (next_heartbeat < now) next_heartbeat = now + dds_lifecycle::kHeartbeatInterval;
        }

        // Sleep until the next release or heartbeat, but poll commands at least every 2 ms.
        auto wake = std::min(now + std::chrono::milliseconds(2), next_heartbeat);
        if (active) wake = std::min(wake, next_tick);
        std::this_thread::sleep_until(wake);
    }
    log::info(name_, "finalized");
    // Give the transient-local status a moment to reach the manager.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return 0;
}

}  // namespace vrm
