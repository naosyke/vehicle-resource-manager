#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "vrm/manifest.hpp"

using vrm::Criticality;
using vrm::ManifestError;
using vrm::parse_manifest;

namespace {

std::vector<std::string> names(const std::vector<const vrm::NodeSpec*>& nodes) {
    std::vector<std::string> result;
    for (const auto* node : nodes) result.push_back(node->name);
    return result;
}

}  // namespace

TEST(Manifest, ParsesNodesAndResources) {
    const auto manifest = parse_manifest(R"(
system: car
transition_timeout_ms: 500
nodes:
  - name: brake
    executable: vrm_demo_node
    args: ["--period-ms", "10"]
    criticality: safety_critical
    priority: 80
    resources: {cpu: 0.5, memory: 64Mi, cpus: [0, 1]}
  - name: radio
    executable: /usr/bin/radio
)");

    EXPECT_EQ(manifest.name, "car");
    EXPECT_EQ(manifest.transition_timeout.count(), 500);
    ASSERT_EQ(manifest.nodes.size(), 2u);

    const auto& brake = manifest.nodes[0];
    EXPECT_EQ(brake.args, (std::vector<std::string>{"--period-ms", "10"}));
    EXPECT_EQ(brake.criticality, Criticality::SafetyCritical);
    EXPECT_EQ(brake.priority, 80);
    EXPECT_DOUBLE_EQ(*brake.resources.cpu_cores, 0.5);
    EXPECT_EQ(*brake.resources.memory_bytes, 64u * 1024 * 1024);
    EXPECT_EQ(brake.resources.cpus, (std::vector<int>{0, 1}));

    const auto& radio = manifest.nodes[1];
    EXPECT_EQ(radio.criticality, Criticality::BestEffort);
    EXPECT_EQ(radio.priority, 0);
    EXPECT_FALSE(radio.resources.cpu_cores.has_value());
    EXPECT_NE(manifest.find("radio"), nullptr);
    EXPECT_EQ(manifest.find("wipers"), nullptr);
}

TEST(Manifest, MemorySizes) {
    EXPECT_EQ(vrm::parse_memory_size("4096"), 4096u);
    EXPECT_EQ(vrm::parse_memory_size("2Ki"), 2048u);
    EXPECT_EQ(vrm::parse_memory_size("1Gi"), 1024ull * 1024 * 1024);
    EXPECT_EQ(vrm::parse_memory_size("512M"), 512ull * 1000 * 1000);
    EXPECT_THROW(vrm::parse_memory_size("lots"), ManifestError);
    EXPECT_THROW(vrm::parse_memory_size("64MB"), ManifestError);
}

TEST(Manifest, RejectsInvalidManifests) {
    EXPECT_THROW(parse_manifest("nodes: 3"), ManifestError);
    EXPECT_THROW(parse_manifest("nodes: [{executable: x}]"), ManifestError);
    EXPECT_THROW(parse_manifest("nodes: [{name: a}]"), ManifestError);
    EXPECT_THROW(parse_manifest("nodes: [{name: a, executable: x, criticality: urgent}]"), ManifestError);
    EXPECT_THROW(parse_manifest("nodes: [{name: a, executable: x, priority: 120}]"), ManifestError);
    EXPECT_THROW(parse_manifest("nodes: [{name: a, executable: x}, {name: a, executable: y}]"), ManifestError);
    EXPECT_THROW(parse_manifest("nodes: [{name: a, executable: x, depends_on: [b]}]"), ManifestError);
    EXPECT_THROW(parse_manifest("nodes: [{name: a, executable: x, resources: {cpu: 0}}]"), ManifestError);
    EXPECT_THROW(parse_manifest("nodes: [unclosed"), ManifestError);
}

TEST(Manifest, StartupOrderPutsCriticalNodesFirst) {
    const auto manifest = parse_manifest(R"(
nodes:
  - {name: radio, executable: x, criticality: best_effort}
  - {name: camera, executable: x, criticality: mission_critical, priority: 10}
  - {name: lidar, executable: x, criticality: mission_critical, priority: 50}
  - {name: brake, executable: x, criticality: safety_critical}
)");

    EXPECT_EQ(names(vrm::startup_order(manifest)),
              (std::vector<std::string>{"brake", "lidar", "camera", "radio"}));
}

TEST(Manifest, StartupOrderRespectsDependencies) {
    const auto manifest = parse_manifest(R"(
nodes:
  - {name: planner, executable: x, criticality: safety_critical, depends_on: [map]}
  - {name: map, executable: x, criticality: best_effort}
  - {name: radio, executable: x, criticality: best_effort}
)");

    EXPECT_EQ(names(vrm::startup_order(manifest)), (std::vector<std::string>{"map", "planner", "radio"}));
}

TEST(Manifest, DependencyCycleIsRejected) {
    const auto manifest = parse_manifest(R"(
nodes:
  - {name: a, executable: x, depends_on: [b]}
  - {name: b, executable: x, depends_on: [a]}
)");

    EXPECT_THROW(vrm::startup_order(manifest), ManifestError);
}

TEST(Manifest, RestartPolicyDefaultsFollowCriticality) {
    const auto manifest = parse_manifest(R"(
heartbeat_timeout_ms: 800
nodes:
  - {name: brake, executable: x, criticality: safety_critical}
  - {name: camera, executable: x, criticality: mission_critical}
  - {name: radio, executable: x, criticality: best_effort}
  - {name: map, executable: x, criticality: best_effort, restart: on-failure, max_restarts: 5}
  - {name: lidar, executable: x, criticality: mission_critical, restart: never}
)");

    EXPECT_EQ(manifest.heartbeat_timeout.count(), 800);
    EXPECT_EQ(manifest.nodes[0].restart, vrm::RestartPolicy::OnFailure);
    EXPECT_EQ(manifest.nodes[1].restart, vrm::RestartPolicy::OnFailure);
    EXPECT_EQ(manifest.nodes[2].restart, vrm::RestartPolicy::Never);
    EXPECT_EQ(manifest.nodes[3].restart, vrm::RestartPolicy::OnFailure);
    EXPECT_EQ(manifest.nodes[3].max_restarts, 5);
    EXPECT_EQ(manifest.nodes[4].restart, vrm::RestartPolicy::Never);
    EXPECT_EQ(manifest.nodes[0].max_restarts, 3);
}

TEST(Manifest, RejectsInvalidRestartSettings) {
    EXPECT_THROW(parse_manifest("nodes: [{name: a, executable: x, restart: always}]"), ManifestError);
    EXPECT_THROW(parse_manifest("nodes: [{name: a, executable: x, max_restarts: -1}]"), ManifestError);
}
