# Architecture

## 1. Overview

`vrm_manager` owns the lifecycle of every node in the system. Nodes are
separate processes that talk to the manager only through DDS; the manager
also starts them as child processes so it can supervise them.

```mermaid
flowchart LR
    manifest["system.yaml<br/>criticality · priority · resources · depends_on"]

    subgraph manager["vrm_manager"]
        order["Startup order<br/>deps → criticality → priority"]
        client["LifecycleClient<br/>(DDS)"]
        supervisor["Process supervision<br/>fork/exec · waitpid"]
    end

    subgraph nodes["Managed nodes (processes)"]
        brake["brake_control<br/>safety_critical"]
        perception["perception<br/>mission_critical"]
        info["infotainment<br/>best_effort"]
    end

    manifest --> order
    order --> client
    order --> supervisor
    client -- "LifecycleCommand" --> nodes
    nodes -- "LifecycleStatus" --> client
    supervisor -- "spawn / reap" --> nodes
```

| Component | Location | Responsibility |
|---|---|---|
| `LifecycleStateMachine` | `src/core/lifecycle.cpp` | States, transitions and callback results (no DDS) |
| Manifest | `src/core/manifest.cpp` | YAML parsing, validation, startup order |
| `ManagedNode` | `src/dds/managed_node.cpp` | Node side: receives commands, runs the state machine, publishes status |
| `LifecycleClient` | `src/manager/lifecycle_client.cpp` | Manager side: sends commands, tracks node status |
| `Manager` | `src/manager/manager.cpp` | Startup, supervision (heartbeat, deadlines, restarts), degradation rules, shutdown |
| `DeadlineMonitor` | `src/core/deadline_monitor.cpp` | Latency, response time and deadline misses of periodic work |

## 2. Node Lifecycle

Based on the ROS 2 managed node design. Transition states run the matching
callback; the result decides the next primary state.

```mermaid
stateDiagram-v2
    [*] --> Unconfigured
    Unconfigured --> Configuring: configure
    Configuring --> Inactive: success
    Configuring --> Unconfigured: failure

    Inactive --> Activating: activate
    Activating --> Active: success
    Activating --> Inactive: failure

    Active --> Deactivating: deactivate
    Deactivating --> Inactive: success
    Deactivating --> Active: failure

    Inactive --> CleaningUp: cleanup
    CleaningUp --> Unconfigured: success
    CleaningUp --> Inactive: failure

    Unconfigured --> ShuttingDown: shutdown
    Inactive --> ShuttingDown: shutdown
    Active --> ShuttingDown: shutdown
    ShuttingDown --> Finalized: success / failure

    Finalized --> [*]
```

Error handling (omitted from the diagram for readability):

| Situation | Result |
|---|---|
| A callback returns `error` or throws, in any transition state | → `ErrorProcessing`, which runs `on_error` |
| `on_error` succeeds | → `Unconfigured` (the node can be configured again) |
| `on_error` fails | → `Finalized` |
| Invalid transition (e.g. `activate` while unconfigured) | Rejected; the state is unchanged |

## 3. Lifecycle Protocol over DDS

Types are defined in [`idl/LifecycleMsgs.idl`](../idl/LifecycleMsgs.idl) and
generated with `idlcxx`. Both topics are keyed by node name, so each node is
its own DDS instance.

| Topic | Type | Direction | QoS | Why |
|---|---|---|---|---|
| `vrm_lifecycle_command` | `LifecycleCommand` | manager → node | Reliable, Volatile, KeepLast 16 | Commands must not be lost, but are meaningless to a node that starts later |
| `vrm_lifecycle_status` | `LifecycleStatus` | node → manager | Reliable, TransientLocal, KeepLast 1 (writer) | A manager that (re)starts late still gets each node's current state |
| `vrm_node_heartbeat` | `NodeHeartbeat` | node → manager | BestEffort, Volatile, KeepLast 1 | Periodic; a lost sample is replaced by the next one, and missing several means the node hangs |

```mermaid
sequenceDiagram
    participant M as vrm_manager
    participant N as brake_control

    M->>N: fork / exec
    N-->>M: status unconfigured (request 0, "started")
    M->>N: command configure (request 1)
    N-->>M: status configuring (request 1)
    N-->>M: status inactive (request 1, success)
    M->>N: command activate (request 2)
    N-->>M: status activating (request 2)
    N-->>M: status active (request 2, success)
```

**Manager id.** Every manager run picks a random 64-bit id and passes it to
the nodes it starts through the `VRM_MANAGER_ID` environment variable. Both
topics are keyed by `(manager_id, node)`: a node ignores commands from other
managers, and a manager ignores status from other systems' nodes. Without
this, two systems that use the same node names on one network (for example
two containers on the same Docker network) would stop each other's nodes,
because DDS discovery connects them automatically. It also keeps request ids
unambiguous when a manager restarts.

**Resend and idempotency.** Because the command topic is volatile, a command
written before DDS discovery has matched the node's reader would be lost.
The manager therefore resends the command every 300 ms until the node replies
with a primary state carrying the same `request_id`. The node remembers the
last request it handled and answers duplicates by republishing its status
instead of running the transition again.

Intermediate states (`configuring`, `activating`, ...) are informational.
With a KeepLast 1 writer they may be coalesced; the manager only waits for the
primary state with the matching `request_id`.

## 4. Startup, Degradation and Shutdown

**Order.** Dependencies start first. Among nodes that are ready, the most
critical start first, then higher priority, then by name.

**Degradation rules.**

| Situation | Behaviour |
|---|---|
| Best-effort or mission-critical node fails to reach Active | Node is shut down and marked `failed`; startup continues |
| A dependency is not Active | Node is `skipped` |
| Safety-critical node fails to reach Active | Startup is aborted, all nodes are shut down, exit code 1 |
| Node process exits while Active | Logged (ERROR for safety-critical), marked `exited` |

**Shutdown** runs in reverse start order: `deactivate`, `shutdown`, wait for
the process to exit (2 s), then `SIGKILL` if needed. Nodes run in their own
process group, so Ctrl-C reaches only the manager, which then stops the nodes
in order.

## 5. Resource Isolation with cgroup v2

Each node runs in its own cgroup, so the kernel accounts and limits its CPU
and memory separately from every other node.

```mermaid
flowchart TB
    root["/sys/fs/cgroup (container root)<br/>subtree_control: +cpu +memory +cpuset"]
    init["init/<br/>processes that were in the root"]
    vrm["vrm/<br/>subtree_control: +cpu +memory +cpuset"]
    brake["brake_control/<br/>cpu.max 50000 100000<br/>memory.max 64Mi<br/>cpuset.cpus 0"]
    info["infotainment/<br/>cpu.max 100000 100000<br/>memory.max 128Mi"]

    root --> init
    root --> vrm
    vrm --> brake
    vrm --> info
```

| Manifest | cgroup file | Effect |
|---|---|---|
| `cpu: 0.5` | `cpu.max` = `50000 100000` | At most 50 ms of CPU time per 100 ms period; then the group is throttled |
| `memory: 64Mi` | `memory.max`, `memory.swap.max = 0` | Allocations beyond the limit trigger reclaim, then the OOM killer inside this group only |
| `cpus: [0]` | `cpuset.cpus` = `0` | The node's threads may only run on CPU 0 |

**Why `init/`.** cgroup v2 does not allow a group to both contain processes
and delegate controllers to child groups ("no internal processes" rule). The
container's processes are therefore moved into `init/` before the controllers
are enabled. The manager only does this at the root of its own cgroup
namespace (a container started with `--cgroupns=private`), never in a shared
cgroup on a host.

**Joining before exec.** The manager creates the node's group first; the
child process writes its own PID to the group's `cgroup.procs` between
`fork()` and `exec()`, so the budget applies from the node's first
instruction. Because the manager is multi-threaded (DDS), that code uses only
async-signal-safe calls (no allocation, no stdio).

**Monitoring.** The manager reads each group's files:

| File | Used for |
|---|---|
| `cpu.stat` `usage_usec` | CPU usage (difference between samples) |
| `cpu.stat` `nr_throttled` | How often `cpu.max` stopped the node |
| `memory.current`, `memory.peak` | Current and peak memory |
| `memory.events` `oom_kill` | Whether an exit was an OOM kill |

When cgroups are not writable (for example an unprivileged container), the
manager logs a warning and runs without enforcement; `--require-cgroups`
turns that into an error.

## 6. Dashboard

With `--status-file PATH`, the manager writes a JSON snapshot every
`--status-interval` seconds (default 1): per node the lifecycle state,
outcome, budget and measured usage, plus the last 100 events. It writes to a
temporary file and renames it, so a reader never sees a partial file.
`dashboard/index.html` polls the file and keeps a 60 second history in the
browser.

```text
vrm_manager ──(write + rename every 1 s)──▶ status/status.json ◀──(fetch every 1 s)── dashboard/index.html
```

## 7. Real-time Scheduling and Supervision

### Scheduling

A node with `priority: 1-99` runs as `SCHED_FIFO` with that priority. The
child sets it with `sched_setscheduler()` between `fork()` and `exec()`
(after joining its cgroup), so the policy applies from the first instruction
and is inherited by the node's DDS threads. A `SCHED_FIFO` task runs as soon
as it is ready and is only preempted by higher real-time priorities, so
normal (`SCHED_OTHER`) tasks on the same CPU cannot delay it.

The node reports the policy the kernel actually applied in its heartbeat;
the manager warns when it differs from the manifest (e.g. no `CAP_SYS_NICE`).

**`cpu.max` does not limit `SCHED_FIFO` tasks.** cgroup v2 CPU bandwidth
control applies to normal tasks only; a runaway real-time task is limited
only by the kernel's global real-time throttling
(`sched_rt_runtime_us` = 95 % per second). The manager therefore only
*monitors* the CPU budget of real-time nodes and warns when it is exceeded.
Acting on it (e.g. demoting the node) is part of phase 4.

**Environment limits.** Docker Desktop runs Linux in a virtual machine
without a `PREEMPT_RT` kernel. `cyclictest` measures kernel wake-up latencies
of up to ~54 ms there, so a few deadline misses remain even with
`SCHED_FIFO` and no competing load. On a real ECU one would use a
`PREEMPT_RT` kernel, isolated CPUs (`isolcpus`, `nohz_full`) and
`cpuset.cpus.partition = isolated` for safety-critical nodes.

### Deadline monitoring

Each activation (tick) of a node's periodic work is measured against its
release time (when it was due):

| Measure | Definition |
|---|---|
| Latency | start − release |
| Response time | end − release |
| Deadline miss | response time > deadline, or a release skipped because the previous tick overran by more than a period |

Nodes without a deadline (`--no-deadline`, batch work) are measured but never
count misses.

### Heartbeat and alive supervision

Every node publishes a `NodeHeartbeat` every 500 ms from process start:
state, scheduling policy, and the timing statistics since the previous
heartbeat. QoS is best effort / volatile / keep last 1: a lost heartbeat is
simply replaced by the next one.

```mermaid
sequenceDiagram
    participant N as node
    participant M as vrm_manager

    loop every 500 ms
        N-->>M: NodeHeartbeat (policy, max response, misses, ...)
    end
    Note over N: node hangs (endless loop)
    Note over M: no heartbeat for heartbeat_timeout_ms (1.5 s)
    M->>N: SIGKILL
    Note over M: restart policy on-failure: restart after 250 ms
    M->>N: fork / exec, configure, activate
```

### Restart policy

| Criticality | Default `restart` |
|---|---|
| safety_critical, mission_critical | `on-failure` |
| best_effort | `never` |

A node that exits unexpectedly, crashes or loses its heartbeat is restarted
after 250 ms, 500 ms, 1 s, 2 s, then 4 s, up to `max_restarts` (default 3).
When a safety-critical node reaches its limit, the manager reports the system
as degraded.

## 8. Planned

Phase 4 adds pressure-based arbitration (PSI) and acting on budget overruns
(including demoting real-time nodes that exceed their CPU budget), so that
lower-criticality nodes are throttled or stopped before they can interfere
with safety-critical ones.
