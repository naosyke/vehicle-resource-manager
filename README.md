# Vehicle Resource Manager

An execution and resource manager for vehicle middleware nodes on Linux, written in C++17
with [Eclipse Cyclone DDS](https://cyclonedds.io/).

It starts the nodes of a vehicle system in order, drives each node through a
managed lifecycle over DDS, and supervises the processes. The goal of the
project is **freedom from interference**: safety-critical functions such as
braking must keep their CPU time and memory, even when a lower-criticality
function such as infotainment misbehaves.

```text
                 ┌──────────────── vrm_manager ─────────────────┐
  system.yaml ─▶ │ lifecycle control · arbitration · monitoring │
                 └───────┬──────────────────┬──────────────┬────┘
                         │ DDS              │ cgroup v2    │ /proc, PSI
                 ┌───────┴───────┐  ┌───────┴──────┐  ┌────┴─────────┐
                 │ brake_control │  │ perception   │  │ infotainment │
                 │ safety, RT 80 │  │ mission, 50  │  │ best effort  │
                 └───────────────┘  └──────────────┘  └──────────────┘
```

> This is an independent learning and portfolio project. It is built only on
> public specifications (ROS 2 managed node design, OMG DDS, Linux cgroup v2,
> publicly available ISO 26262 / AUTOSAR concepts) and does not contain
> proprietary code, specifications or information from any employer.

## Features

* **Managed node lifecycle** following the [ROS 2 managed node design](https://design.ros2.org/articles/node_lifecycle.html):
  `unconfigured → inactive → active → finalized`, with transition states and
  error processing
* **Lifecycle protocol over DDS**: commands and status as IDL types, with QoS
  chosen per topic (see [docs/architecture.md](docs/architecture.md))
* **System manifest** (YAML): criticality, real-time priority, CPU / memory
  budgets, CPU affinity and dependencies per node
* **Ordered startup and shutdown**: dependencies first, then by criticality and
  priority; shutdown in reverse order
* **Degradation rules**: a best-effort node that fails is stopped and its
  dependents are skipped; a safety-critical node that fails aborts startup
* **Process supervision**: detects nodes that exit or crash unexpectedly

## Requirements

* Docker (the manager uses Linux features: cgroup v2, SCHED_FIFO, /proc).
  Everything builds and runs in the provided image, also on macOS.

## Build and Test

Build the development image (Ubuntu 24.04 + Cyclone DDS C / C++, built from source):

```bash
docker build --target base -t vrm-base .
```

Build and run the tests inside it:

```bash
docker run --rm -v "$PWD":/workspace vrm-base bash -c \
  "cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure"
```

The tests include unit tests (GoogleTest) for the lifecycle state machine and
the manifest, and end-to-end tests that start real node processes over DDS.

## Run the Demo System

```bash
docker run --rm -it -v "$PWD":/workspace vrm-base build/vrm_manager config/system.yaml
```

Press Ctrl-C to shut the system down in order. Add `--exit-after 5` to stop
automatically after 5 seconds.

```text
17:40:36.450 INFO  [manager] spawned brake_control (pid 31, safety_critical)
17:40:36.460 INFO  [manager] brake_control: unconfigured (started)
17:40:36.465 INFO  [manager] brake_control: configuring
17:40:36.465 INFO  [manager] brake_control: inactive
17:40:36.470 INFO  [manager] brake_control: activating
17:40:36.470 INFO  [manager] brake_control: active
...
NODE                 CRITICALITY       PRIO     PID OUTCOME      DETAIL
brake_control        safety_critical     80      31 active
perception           mission_critical    50      39 active
path_planning        mission_critical    40      47 active
infotainment         best_effort          0      55 active
telemetry_uploader   best_effort          0      63 active
```

Try the failure scenarios:

```bash
# A best-effort node fails to activate; its dependent is skipped, the system keeps running.
docker run --rm -v "$PWD":/workspace vrm-base build/vrm_manager config/test_system.yaml --exit-after 2

# A safety-critical node fails to configure; startup is aborted (exit code 1).
docker run --rm -v "$PWD":/workspace vrm-base build/vrm_manager config/test_safety_failure.yaml
```

## System Manifest

```yaml
system: demo_vehicle
transition_timeout_ms: 3000

nodes:
  - name: brake_control
    executable: vrm_demo_node          # Resolved next to vrm_manager, then PATH
    args: ["--period-ms", "10", "--work-ms", "2"]
    criticality: safety_critical       # safety_critical | mission_critical | best_effort
    priority: 80                       # 1-99 = SCHED_FIFO priority, 0 = normal
    resources:
      cpu: 0.5                         # CPU cores (cgroup cpu.max)
      memory: 64Mi                     # Hard limit (cgroup memory.max)
      cpus: [0]                        # CPU affinity (cpuset.cpus)
    depends_on: []
```

`vrm_demo_node` models a vehicle function: `--period-ms` and `--work-ms` set
its periodic CPU load, `--memory-mb` the memory it holds, and `--fail-on`
makes a transition fail for testing.

## Project Structure

```text
vehicle-resource-manager/
├── idl/LifecycleMsgs.idl        # DDS types: LifecycleCommand, LifecycleStatus
├── include/vrm/                 # Public headers
├── src/
│   ├── core/                    # Lifecycle state machine, manifest, logging (no DDS)
│   ├── dds/                     # DDS QoS / conversions, ManagedNode base class
│   ├── manager/                 # vrm_manager: startup, supervision, shutdown
│   └── nodes/demo_node.cpp      # Configurable demo node
├── config/                      # System manifests (demo and tests)
├── tests/                       # GoogleTest unit tests
├── docs/architecture.md
├── CMakeLists.txt
└── Dockerfile
```

## Roadmap

### Phase 1 - Lifecycle and DDS

* [x] Managed node lifecycle state machine
* [x] Lifecycle commands and status over Cyclone DDS
* [x] System manifest with criticality, priority, resources and dependencies
* [x] Ordered startup / shutdown and process supervision

### Phase 2 - Resource Budgets

* [ ] Put each node in its own cgroup v2 group
* [ ] Enforce CPU (`cpu.max`), memory (`memory.max`) and CPU affinity (`cpuset.cpus`)
* [ ] Measure actual CPU and memory usage per node

### Phase 3 - Real-time and Supervision

* [ ] SCHED_FIFO priorities for real-time nodes
* [ ] Heartbeat (alive supervision) and deadline monitoring over DDS
* [ ] Restart policies per criticality

### Phase 4 - Arbitration

* [ ] Detect resource pressure (PSI) and budget overruns
* [ ] Degrade lower-criticality nodes (throttle, deactivate, stop) to protect safety-critical ones
* [ ] Demo: a runaway infotainment node does not make brake_control miss a deadline

### Phase 5 - Observability

* [ ] Resource and lifecycle telemetry over DDS
* [ ] Live view of nodes, budgets and deadline statistics
