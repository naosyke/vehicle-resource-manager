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
| `Manager` | `src/manager/manager.cpp` | Startup, supervision, degradation rules, shutdown |

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

## 5. Planned: Resource Isolation

Phase 2 onward adds per-node cgroup v2 groups (`cpu.max`, `memory.max`,
`cpuset.cpus`), SCHED_FIFO priorities, heartbeat / deadline monitoring and
pressure-based arbitration, so that lower-criticality nodes cannot interfere
with safety-critical ones.
