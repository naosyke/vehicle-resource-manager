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
* **CPU shares by criticality**: `cpu.weight` 10000 / 1000 / 10 for safety /
  mission / best-effort nodes, so critical nodes get the CPU they need while
  less critical ones still use whatever is left
* **Resource arbitration**: when a protected node waits for CPU (PSI) or keeps
  missing deadlines, less critical nodes on the same CPUs yield (lower
  weight), are throttled, deactivated and finally stopped - only as far as
  needed - and restored when things calm down. Nodes near their memory limit
  are stopped gracefully before the OOM killer strikes; runaway `SCHED_FIFO`
  nodes are demoted
* **Live dashboard**: CPU core map, usage against budget, response time
  against deadline, arbitration state and events in the browser
* **CPU timeline**: which node runs on which CPU, from the kernel's
  `sched_switch` trace events, down to a 100 ms window

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
| `--no-arbitration` | Never throttle, deactivate or stop nodes to protect others |
| `--equal-weights` | `cpu.weight` 100 for every node instead of by criticality |
| `--status-file PATH` | Write a JSON status snapshot for the dashboard |
| `--status-interval SECONDS` | Status snapshot interval (default 1) |
| `--trace-cpus LIST` | Add a timeline of which node runs on these CPUs (e.g. `0,1`), from ftrace |

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
  build/vrm_manager config/demo_cpu_contention.yaml --no-rt --no-arbitration --exit-after 30
docker run --rm --privileged --cgroupns=private -v "$PWD":/workspace vrm-base \
  build/vrm_manager config/demo_cpu_contention.yaml --no-arbitration --exit-after 30
```

Measured over 30 s (3000 activations) with Docker Desktop on macOS,
arbitration off to isolate the effect of scheduling:

| Setup | Scheduling | Deadline misses | Max release latency (last 0.5 s) |
|---|---|---|---|
| brake_control alone on CPU 0 | SCHED_FIFO 80 | 0 | 0.73 ms |
| with 2 CPU hogs on CPU 0 | SCHED_OTHER (`--no-rt`) | **68** | 1.68 ms |
| with 2 CPU hogs on CPU 0 | SCHED_FIFO 80 | **0** | **0.01 ms** |

The virtual machine itself occasionally stalls: `cyclictest` in the same
container measures kernel wake-up latencies of up to 54 ms, so single misses
can appear in any setup. Hard real-time needs native Linux with a
`PREEMPT_RT` kernel and isolated CPUs; see
[docs/architecture.md](docs/architecture.md#7-real-time-scheduling-and-supervision).

### Demo: a schedulable system - every node meets its deadline

[`config/demo_schedulable.yaml`](config/demo_schedulable.yaml) is a realistic
mix on CPU 0 that needs 90 % of it in total:

| Node | Period | Work | Deadline | CPU | Scheduling |
|---|---|---|---|---|---|
| brake_control | 10 ms | 2 ms | 4 ms | 20 % | SCHED_FIFO 80 |
| perception | 50 ms | 20 ms | 30 ms | 40 % | normal, cpu.weight 1000 |
| infotainment | 100 ms | 15 ms | 100 ms | 15 % | normal, cpu.weight 10 |
| map_renderer | 200 ms | 30 ms | 200 ms | 15 % | normal, cpu.weight 10 |

```bash
docker run --rm --privileged --cgroupns=private -v "$PWD":/workspace vrm-base \
  build/vrm_manager config/demo_schedulable.yaml --exit-after 30
```

Deadline misses over 30 s:

| Setup | brake_control (3000) | perception (600) | infotainment (300) | map_renderer (150) |
|---|---|---|---|---|
| default: SCHED_FIFO + weights by criticality | **1** | **2** | **0** | **0** |
| `--equal-weights --no-arbitration` | 0 | **453** | 0 | 0 |
| `--no-rt --equal-weights --no-arbitration` | **595** | **451** | 0 | 0 |

The CPU is sufficient on average, but the tight deadlines only hold when the
critical nodes run first: with equal shares perception's 20 ms of work is
interleaved with the others and takes 52 ms; with its higher weight it runs
in one go and finishes in 28 ms. The best-effort nodes meet their looser
deadlines in the gaps either way. (Results vary between runs in the Docker
Desktop VM: in a run with many VM stalls brake_control, which cannot be
delayed by the other nodes, missed 24 deadlines and the others a few.)

### Demo: watching the scheduler on two CPUs

[`config/demo_two_cores.yaml`](config/demo_two_cores.yaml) pins brake_control
to CPU 1 and lets perception and two CPU hogs run on CPU 0 or 1. With
`--trace-cpus` the manager records the kernel's `sched_switch` events (ftrace)
for those CPUs and the dashboard shows which node ran when:

```bash
docker run --rm -it --privileged --cgroupns=private -v "$PWD":/workspace vrm-base \
  build/vrm_manager config/demo_two_cores.yaml --trace-cpus 0,1 \
  --status-file status/status.json --status-interval 0.25 --report-interval 0
```

At 200 ms zoom the timeline shows brake_control preempting whatever runs on
CPU 1 for 2 ms every 10 ms, perception running in 20 ms bursts every 50 ms,
and the hogs filling the gaps. Nodes name their main thread after
themselves (`prctl(PR_SET_NAME)`), so the trace can tell them apart; DDS
helper threads and system tasks appear as "other".

### Demo: protecting a mission-critical node without stopping anyone

In [`config/demo_arbitration.yaml`](config/demo_arbitration.yaml), perception
(mission-critical, normal scheduling) needs 20 ms of CPU every 50 ms (40 %)
with a 30 ms deadline, on CPU 0 next to brake_control (`SCHED_FIFO`, 20 %) and
two best-effort CPU hogs. With equal shares it would get only about 27 %.

```bash
docker run --rm --privileged --cgroupns=private -v "$PWD":/workspace vrm-base \
  build/vrm_manager config/demo_arbitration.yaml --exit-after 30
```

30 s each (perception has 600 activations):

| Setup | perception misses | perception CPU | Hogs (each) | Arbitration actions |
|---|---|---|---|---|
| `--equal-weights --no-arbitration` | **600** (all) | 26.6 % | 26.6 % | - |
| `--equal-weights` (arbitration only) | 66 (while reacting) | 39.9 % | shares the rest | weights lowered |
| default: weights by criticality | **0** | **41.2 %** | **19.2 %** | **none needed** |

With `cpu.weight` by criticality the CPU is fully used and nobody is stopped:
perception gets its 40 % whenever it runs, and the hogs use what is left.
Arbitration is the fallback when shares are not enough; it starts by lowering
the weight (which still lets the node use idle CPU) and only then throttles,
deactivates or stops.

Scheduling priority and shares *prevent* interference; arbitration
*contains* the cases they do not cover.
The same run also shows two other actions: `config/demo_memory_leak.yaml` now
stops infotainment at 91 % of its memory limit instead of waiting for the OOM
killer, and `config/test_rt_runaway.yaml` demotes a `SCHED_FIFO` node that uses
80 % CPU against a 30 % budget, after which cpu.max holds it at 29 %.

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
its 10 ms period because its memory and CPU are accounted separately. With
arbitration (phase 4, on by default) the manager stops infotainment gracefully
at 90 % of its limit, before the OOM killer has to; the output above is from
`--no-arbitration`.

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

arbitration:                           # All optional; these are the defaults
  enabled: true
  cpu_pressure_threshold: 0.25         # Protected node waits for CPU > 25%: others yield
  miss_rounds: 2                       # Misses in 2 rounds (0.5 s) in a row: full ladder
  escalation_interval_s: 2             # Time between degradation steps
  recovery_s: 5                        # Calm time before restoring one step (doubles on relapse)
  lowered_cpu_weight: 1                # cpu.weight of yielding nodes
  throttle_cpu: 0.1                    # cpu.max of throttled nodes
  memory_stop_fraction: 0.9            # Stop gracefully at 90% of memory.max
  rt_overrun_s: 1                      # SCHED_FIFO over its CPU budget this long: demote

nodes:
  - name: brake_control
    executable: vrm_demo_node          # Resolved next to vrm_manager, then PATH
    args: ["--period-ms", "10", "--work-ms", "2"]
    criticality: safety_critical       # safety_critical | mission_critical | best_effort
    priority: 80                       # 1-99 = SCHED_FIFO priority, 0 = normal
    restart: on-failure                # on-failure | never (default by criticality)
    max_restarts: 3
    resources:
      cpu: 0.5                         # CPU cores (cgroup cpu.max): hard cap
      cpu_weight: 10000                # Share when competing (cpu.weight); default by criticality
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

* [x] Detect resource pressure (PSI), persistent deadline misses and budget overruns
* [x] CPU shares (`cpu.weight`) by criticality
* [x] Degrade lower-criticality nodes (lower weight, throttle, deactivate, stop) and restore them with hysteresis
* [x] Stop nodes before the OOM killer; demote runaway SCHED_FIFO nodes
* [x] Demos: CPU contention with and without arbitration, memory leak, real-time runaway

### Phase 5 - Observability

* [x] Live view of nodes, budgets, usage and events (JSON status + dashboard)
* [x] Deadline statistics in the dashboard
* [ ] Resource and lifecycle telemetry over DDS
