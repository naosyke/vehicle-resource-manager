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
* **Resource budgets enforced with cgroup v2**: each node gets its own group
  with `cpu.max`, `memory.max` and `cpuset.cpus`; the process joins the group
  before `exec`, so limits apply from its first instruction
* **Resource monitoring**: per-node CPU usage, memory, peak memory, CPU
  throttling and OOM kills, read from the cgroup files
* **Real-time scheduling**: nodes with a priority run as `SCHED_FIFO`, set
  between `fork` and `exec`; the manager checks the policy the kernel really
  applied
* **Deadline monitoring**: every node measures release latency, response time
  and deadline misses of its periodic work and reports them in a heartbeat
* **Alive supervision and restarts**: a node whose heartbeat stops is treated
  as hung and killed; crashed or hung nodes are restarted with backoff
  according to their restart policy
* **Live dashboard**: CPU core map, usage against budget, response time
  against deadline, and events in the browser

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
docker run --rm --privileged --cgroupns=private -v "$PWD":/workspace vrm-base bash -c \
  "cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure"
```

The tests include unit tests (GoogleTest) for the lifecycle state machine,
the manifest and the cgroup helpers, end-to-end tests that start real node
processes over DDS, and tests that check the kernel really enforces the
budgets (a memory hog is OOM-killed at its limit, a busy loop is throttled to
its CPU quota, a process is pinned to its CPU).

`--privileged --cgroupns=private` gives the container its own, writable cgroup
tree. Without it the enforcement tests are skipped and the manager runs
without enforcing budgets (it logs a warning).

## Run the Demo System

```bash
docker run --rm -it --privileged --cgroupns=private -v "$PWD":/workspace vrm-base \
  build/vrm_manager config/system.yaml
```

Press Ctrl-C to shut the system down in order.

| Option | Description |
|---|---|
| `--exit-after SECONDS` | Stop automatically after this many seconds |
| `--report-interval SECONDS` | Resource report interval (default 5, 0 = off) |
| `--no-cgroups` | Do not enforce resource budgets |
| `--require-cgroups` | Exit with code 3 if budgets cannot be enforced |
| `--no-rt` | Run every node with normal scheduling (for comparisons) |
| `--status-file PATH` | Write a JSON status snapshot for the dashboard |

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

### Demo: real-time priority protects the brake's deadline

In [`config/demo_cpu_contention.yaml`](config/demo_cpu_contention.yaml), two
CPU hogs share CPU 0 with brake_control, which has 2 ms of work every 10 ms and
a 4 ms deadline. Compare normal scheduling with `SCHED_FIFO`:

```bash
docker run --rm --privileged --cgroupns=private -v "$PWD":/workspace vrm-base \
  build/vrm_manager config/demo_cpu_contention.yaml --no-rt --exit-after 30
docker run --rm --privileged --cgroupns=private -v "$PWD":/workspace vrm-base \
  build/vrm_manager config/demo_cpu_contention.yaml --exit-after 30
```

Measured over 30 s (about 3000 activations) with Docker Desktop on macOS:

| Setup | Scheduling | Deadline misses | Max release latency |
|---|---|---|---|
| brake_control alone on CPU 0 | SCHED_FIFO 80 | 16 | 0.37 ms |
| with 2 CPU hogs on CPU 0 | SCHED_OTHER (`--no-rt`) | **342** (11 %) | 2.48 ms |
| with 2 CPU hogs on CPU 0 | SCHED_FIFO 80 | **9** (0.3 %) | **0.01 ms** |

With `SCHED_FIFO` the hogs cause no additional misses: the remaining misses
also happen with brake_control alone. They come from the virtual machine, not
from other Linux tasks - `cyclictest` in the same container measures kernel
wake-up latencies of up to 54 ms. Hard real-time needs native Linux with a
`PREEMPT_RT` kernel and isolated CPUs; see
[docs/architecture.md](docs/architecture.md#7-real-time-scheduling-and-supervision).

### Demo: a memory leak does not affect the brake

In [`config/demo_memory_leak.yaml`](config/demo_memory_leak.yaml), infotainment
leaks 40 MiB/s until it reaches its 128 MiB limit:

```bash
docker run --rm --privileged --cgroupns=private -v "$PWD":/workspace vrm-base \
  build/vrm_manager config/demo_memory_leak.yaml --exit-after 6 --report-interval 1.5
```

```text
RESOURCES                CPU  LIMIT    MEMORY      PEAK     LIMIT THROTTLED  OOM
brake_control          20.4%    50%     1.6Mi     1.7Mi    64.0Mi         0    0
infotainment           16.2%   100%    94.7Mi    94.7Mi   128.0Mi         0    0

WARN  [manager] infotainment (best_effort) killed by the OOM killer (memory limit 128.0Mi) unexpectedly

RESOURCES                CPU  LIMIT    MEMORY      PEAK     LIMIT THROTTLED  OOM
brake_control          20.8%    50%     1.4Mi     1.7Mi    64.0Mi         0    0
infotainment            0.0%   100%   428.0Ki   128.0Mi   128.0Mi         0    1
```

The kernel stops infotainment at exactly its limit, and brake_control keeps
its 10 ms period (605 ticks in 6 seconds) because its memory and CPU are
accounted separately. In phase 4 the manager will act on the rising memory
before the OOM killer has to.

## Dashboard

The manager can write a JSON snapshot of all nodes (state, budgets, usage,
recent events) every second, and `dashboard/index.html` shows it live.

```bash
# Terminal 1 (in the repository root): run the system and write status/status.json
docker run --rm -it --privileged --cgroupns=private -v "$PWD":/workspace vrm-base \
  build/vrm_manager config/system.yaml --report-interval 0 --status-file status/status.json

# Terminal 2 (in the repository root): serve it and open http://localhost:8080/dashboard/
python3 -m http.server 8080
```

* **CPU cores**: which node is pinned to which core, and which nodes may run
  on any core. A pinned core that unpinned nodes can also use is marked as
  shared.
* **Nodes**: CPU and memory as bars against each node's budget (the gray tick
  is peak memory), with the last 60 seconds as charts where the dashed line
  is the limit. A node near its memory limit, throttled nodes and OOM kills
  are flagged.
* **Events**: lifecycle changes, failures and OOM kills.

Run `config/demo_memory_leak.yaml` the same way to watch infotainment approach
its limit, get OOM-killed, and brake_control carry on.

### Failure scenarios

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
heartbeat_timeout_ms: 1500             # No heartbeat for this long = hung

nodes:
  - name: brake_control
    executable: vrm_demo_node          # Resolved next to vrm_manager, then PATH
    args: ["--period-ms", "10", "--work-ms", "2"]
    criticality: safety_critical       # safety_critical | mission_critical | best_effort
    priority: 80                       # 1-99 = SCHED_FIFO priority, 0 = normal
    restart: on-failure                # on-failure | never (default by criticality)
    max_restarts: 3
    resources:
      cpu: 0.5                         # CPU cores (cgroup cpu.max)
      memory: 64Mi                     # Hard limit (cgroup memory.max)
      cpus: [0]                        # CPU affinity (cpuset.cpus)
    depends_on: []
```

`vrm_demo_node` models a vehicle function:

| Option | Effect |
|---|---|
| `--period-ms`, `--work-ms` | Periodic CPU load: `work` ms of computation every `period` ms |
| `--deadline-ms` / `--no-deadline` | Deadline of each activation (default: the period) / batch work without a deadline |
| `--memory-mb`, `--leak-mb-per-sec` | Memory held, and memory leaked while active |
| `--crash-after-sec`, `--hang-after-sec` | Crash (SIGABRT) or stop responding after activation |
| `--fail-on` | Make a lifecycle transition fail |

## Project Structure

```text
vehicle-resource-manager/
├── idl/LifecycleMsgs.idl        # DDS types: LifecycleCommand, LifecycleStatus
├── include/vrm/                 # Public headers
├── src/
│   ├── core/                    # Lifecycle state machine, manifest, cgroups, logging (no DDS)
│   ├── dds/                     # DDS QoS / conversions, ManagedNode base class
│   ├── manager/                 # vrm_manager: startup, supervision, shutdown
│   └── nodes/demo_node.cpp      # Configurable demo node
├── config/                      # System manifests (demo and tests)
├── dashboard/index.html         # Live resource dashboard (reads status/status.json)
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

* [x] Put each node in its own cgroup v2 group
* [x] Enforce CPU (`cpu.max`), memory (`memory.max`) and CPU affinity (`cpuset.cpus`)
* [x] Measure actual CPU and memory usage per node

### Phase 3 - Real-time and Supervision

* [x] SCHED_FIFO priorities for real-time nodes
* [x] Heartbeat (alive supervision) and deadline monitoring over DDS
* [x] Restart policies per criticality

### Phase 4 - Arbitration

* [ ] Detect resource pressure (PSI) and budget overruns
* [ ] Degrade lower-criticality nodes (throttle, deactivate, stop) to protect safety-critical ones
* [ ] Demo: a runaway infotainment node does not make brake_control miss a deadline

### Phase 5 - Observability

* [x] Live view of nodes, budgets, usage and events (JSON status + dashboard)
* [x] Deadline statistics in the dashboard
* [ ] Resource and lifecycle telemetry over DDS
