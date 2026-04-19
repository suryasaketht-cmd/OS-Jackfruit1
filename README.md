
# Multi-Container Runtime and Kernel Memory Monitor

This repository contains a lightweight Linux container runtime in C with:

- a long-running parent supervisor in user space
- a control-plane CLI over UNIX domain sockets
- concurrent bounded-buffer log capture for container stdout and stderr
- a Linux kernel module that enforces soft and hard memory limits
- scheduling experiment scripts and cleanup checks

This README is organized to match the assignment rubric in project-guide.md.

## 1. Team Information

- Member 1: Siddanth Anil, PES1UG24AM274
- Member 2: Srivamshi Chikkireddit, PES1UG24AM87

## 2. Build, Load, and Run Instructions

### 2.1 Environment

- Ubuntu 22.04 or 24.04 VM
- Secure Boot OFF
- Not WSL

Install dependencies:

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
````

Optional preflight check:

```bash
cd boilerplate
chmod +x environment-check.sh
sudo ./environment-check.sh
```

### 2.2 Root Filesystem Setup

From repo root:

```bash
cd boilerplate
mkdir -p rootfs-base
wget -O alpine-minirootfs-3.20.3-x86_64.tar.gz \
  [https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz](https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz)
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# One writable copy per container
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

### 2.3 Build

CI-safe user-space compile:

```bash
make -C boilerplate ci
```

Full build (user space + kernel module):

```bash
cd boilerplate
make
```

### 2.4 Load Kernel Module and Verify Device

```bash
cd boilerplate
sudo insmod monitor.ko
ls -l /dev/container_monitor
```

### 2.5 Start Supervisor and Use CLI

Start supervisor in one terminal:

```bash
cd boilerplate
sudo ./engine supervisor ./rootfs-base
```

In another terminal:

```bash
cd boilerplate
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80 --nice 0
sudo ./engine start beta  ./rootfs-beta  /bin/sh --soft-mib 64 --hard-mib 96 --nice 0

sudo ./engine ps
sudo ./engine logs alpha
sudo ./engine stop alpha
sudo ./engine stop beta
```

Foreground run mode example:

```bash
cd boilerplate
sudo ./engine run gamma ./rootfs-alpha "/bin/sh -c 'echo hello; exit 7'"
echo "engine run exit code: $?"
```

### 2.6 Workload Setup for Experiments

Build workloads:

```bash
cd boilerplate
make
```

Copy workload binaries into container rootfs copies before launch:

```bash
cp ./cpu_hog ./rootfs-alpha/
cp ./io_pulse ./rootfs-beta/
cp ./memory_hog ./rootfs-alpha/
```

### 2.7 Task 5 and Task 6 Scripts

Task 5 scheduler experiments:

```bash
cd boilerplate
sudo ./task5_experiments.sh
```

Expected output artifacts:

  - experiments/task5\_results.csv
  - logs/e1\_high.log
  - logs/e1\_low.log
  - logs/e2\_cpu.log
  - logs/e2\_io.log

Task 6 cleanup verification:

```bash
cd boilerplate
sudo ./task6_cleanup_check.sh
```

### 2.8 Shutdown and Cleanup

```bash
cd boilerplate
sudo pkill -f "./engine supervisor" || true
sudo rmmod monitor
make clean
```

The cleanup path removes the control socket, build artifacts, logs, and generated binaries. If you created extra writable rootfs copies for experiments, remove those as well before packaging the repository.

## 3\. Demo with Screenshots

### Screenshot 1 — Multi-container supervision

*Two containers (alpha, beta) running under one supervisor process, shown via `./engine ps`.*
![Screenshot 1](docs/screenshots/ss1.jpeg)

-----

### Screenshot 2 — Metadata tracking

*Output of `./engine ps` showing all tracked container metadata: id, pid, state, soft/hard limits, exit code, signal, log path.*
![Screenshot 2](docs/screenshots/ss2.jpeg)

-----

### Screenshot 3 — Bounded-buffer logging

*Log file contents captured through the pipe → bounded buffer → consumer thread pipeline. `cat logs/alpha.log` shows cpu\_hog output routed through the logging pipeline.*
![Screenshot 3](docs/screenshots/ss3.jpeg)

-----

### Screenshot 4 — CLI and IPC

*`./engine stop alpha` issued from CLI client, request sent over UNIX domain socket to supervisor, supervisor responds "OK stop requested id=alpha" and updates container state to stopped.*
![Screenshot 4](docs/screenshots/ss4.jpeg)

-----

### Screenshot 5 — Soft-limit warning

*`dmesg` output showing `[container_monitor] Registering container=memtest` and `HARD LIMIT` exceeded event logged by the kernel module when container RSS crosses the soft limit threshold.*
![Screenshot 5](docs/screenshots/ss5.jpeg)

-----

### Screenshot 6 — Hard-limit enforcement

*Container `memtest` started with `--soft-mib 40 --hard-mib 64`, kernel module detects RSS exceeding hard limit and sends SIGKILL. `./engine ps` shows state as `hard_limit_killed` with exit signal 9.*
![Screenshot 6](docs/screenshots/ss6.jpeg)

-----

### Screenshot 7 — Scheduling experiment

*Two CPU-bound containers launched: `cpuA` at nice=0, `cpuB` at nice=15. `top` output shows cpuA receiving \~79% CPU vs cpuB receiving \~44%, demonstrating CFS weighted scheduling based on nice values.*
![Screenshot 7](docs/screenshots/ss7.jpeg)

-----

### Screenshot 8 — Clean teardown

*After stopping all containers and sending SIGINT to supervisor, `ps aux | grep engine` shows no engine supervisor processes remaining. No zombie processes.*
![Screenshot 8](docs/screenshots/ss8.jpeg)

-----

## 4\. Engineering Analysis

### 4.1 Isolation Mechanisms

The runtime creates containers with clone using CLONE\_NEWPID, CLONE\_NEWUTS, and CLONE\_NEWNS.
Inside the child, the mount namespace is privatized and the process chroots into the container-specific rootfs.
Then proc is mounted inside that namespace so tools such as ps operate on container PID view.
The kernel itself remains shared among all containers, so this is OS-level isolation, not VM-level isolation.

### 4.2 Supervisor and Process Lifecycle

The supervisor is a persistent parent that accepts control requests from short-lived CLI clients.
It tracks container metadata, reaps children via SIGCHLD + waitpid, and records terminal states.
Manual stop requests set stop\_requested before signaling the container, enabling clear attribution between manual stop and hard-limit kill.

### 4.3 IPC, Threads, and Synchronization

The design uses two separate IPC paths.

  - Control plane: UNIX domain socket at /tmp/mini\_runtime.sock for CLI to supervisor commands
  - Logging plane: per-container pipe from child stdout and stderr to supervisor producer thread

Producer threads push chunks into a bounded buffer protected by mutex + condition variables.
Consumer thread pops chunks and appends to per-container log files.
Without these primitives, races can corrupt buffer indices, lose log chunks, or deadlock producers and consumers.

### 4.4 Memory Management and Enforcement

The kernel module periodically measures RSS of monitored PIDs.
RSS indicates resident physical pages but not all virtual memory mappings.
Soft limit emits one warning per entry when first exceeded.
Hard limit triggers SIGKILL and removal from monitor list.
Enforcement belongs in kernel space because process memory accounting and reliable kill authority are kernel responsibilities.

### 4.5 Scheduling Behavior

Experiments compare priority-differentiated CPU-bound containers and mixed CPU/I-O workloads.
Observed completion differences are consistent with Linux CFS goals:

  - higher-priority (lower nice) CPU workload receives greater CPU share
  - I-O-bound workload remains responsive due to frequent sleeps/yields
  - fairness and throughput depend on runnable mix and priority weights

## 5\. Design Decisions and Tradeoffs

1.  Namespace + chroot isolation
       - Decision: CLONE\_NEWPID/NEWUTS/NEWNS + chroot + proc mount.
       - Tradeoff: simpler than pivot\_root, but chroot can be less strict against some escape patterns.
       - Justification: faster implementation with required assignment isolation behavior.

2.  Supervisor architecture
       - Decision: one long-running daemon and short-lived command clients.
       - Tradeoff: requires explicit IPC protocol and lifecycle management.
       - Justification: clean separation of stateful control and stateless CLI UX.

3.  Logging and bounded buffer
       - Decision: producer/consumer with mutex + condition variables.
       - Tradeoff: more complexity than direct writes, but avoids slow I/O blocking producers.
       - Justification: robust concurrent capture with bounded memory and controlled shutdown.

4.  Kernel monitor locking
       - Decision: mutex over shared monitored list.
       - Tradeoff: potential contention during timer traversal.
       - Justification: ioctl and timer paths are process context and can sleep; mutex keeps implementation simple and safe.

5.  Scheduler experiments
       - Decision: automated scripts with repeatable container IDs and CSV output.
       - Tradeoff: synthetic workloads may not represent all real-world behavior.
       - Justification: reproducible, rubric-aligned comparisons with measurable outcomes.

## 6\. Scheduler Experiment Results

Run:

```bash
cd boilerplate
sudo ./task5_experiments.sh
cat experiments/task5_results.csv
```

Two containers ran `while true; do ./cpu_hog; done` simultaneously:

  - `cpuA`: nice=0
  - `cpuB`: nice=15

| Container | Nice | Observed %CPU (top) |
|-----------|------|---------------------|
| cpuA      | 0    | \~79.1%              |
| cpuB      | 15   | \~44.9%              |

CFS weight for nice=0 is 1024, for nice=15 is 83. Expected share for cpuA = 1024/(1024+83) ≈ 92.5%. The observed \~79% vs \~44% split reflects this weighting directionally, with the remainder going to kernel threads and system processes.

**Conclusion:** CFS correctly allocates more CPU time to the higher-priority (lower nice) container. The nice=15 container is not starved — it still receives substantial CPU — which demonstrates CFS's fairness guarantee. An I/O-bound container at the same nice level as a CPU-bound container would receive disproportionately more CPU per unit of time it is runnable, because CFS rewards tasks that sleep (yield CPU) with lower virtual runtime.

## 7\. File Map

  - boilerplate/engine.c: user-space supervisor, CLI control path, logging pipeline, namespace launch
  - boilerplate/monitor.c: kernel monitor module, memory checks, soft/hard enforcement
  - boilerplate/monitor\_ioctl.h: shared ioctl request and command IDs
  - boilerplate/cpu\_hog.c, boilerplate/io\_pulse.c, boilerplate/memory\_hog.c: workloads
  - boilerplate/task5\_experiments.sh: scheduler experiment automation
  - boilerplate/task6\_cleanup\_check.sh: teardown and zombie checks

## 8\. Notes for Evaluators

  - Use unique writable rootfs directories for concurrent live containers.
  - engine run returns the child exit code (or 128 + signal for signaled exit).
  - stop attribution is tracked with stop\_requested and shown in metadata state.

## 9\. Submission Checklist

  - `make -C boilerplate ci` completes successfully.
  - The required source files are present: `boilerplate/engine.c`, `boilerplate/monitor.c`, `boilerplate/monitor_ioctl.h`, `boilerplate/Makefile`, and the workload programs.
  - The demo screenshots are present under `docs/screenshots/` and match the rubric items.
  - The task scripts are included and documented: `boilerplate/task5_experiments.sh` and `boilerplate/task6_cleanup_check.sh`.
  - The README includes build, run, cleanup, design, and experiment sections needed for grading.

<!-- end list -->


```
