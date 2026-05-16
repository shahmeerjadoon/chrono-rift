# Chrono Rift

**CS 2006 - Operating Systems | Spring 2026**

Technical Report: Process Architecture, Scheduling, and IPC

**Haris Zahid (24i-0643) · Shahmeer Jadoon (24i-0879)**

---

## Build and Run

The repository contains the full source tree, a Makefile, a Dockerfile, and a `requirements.txt`. Run `make` from the project root to build. This produces three binaries: `arbiter_exec`, `hip_exec`, and `asp_exec`. Run `./arbiter_exec` to start the game. It spawns the other two processes on its own.

The zip file follows the course naming rule: `BCS Section_RollNumber_FullName`.

---

## Screenshots

> **How to add screenshots:**
> 1. Place your image files in the repo (e.g. a `screenshots/` folder).
> 2. Reference them in Markdown like this:
>    ```markdown
>    ![Caption](screenshots/your-image.png)
>    ```
> 3. On GitHub, you can also drag-and-drop images directly into the README editor — it auto-uploads and inserts the link.

### Gameplay

![Gameplay screenshot](screenshots/gameplay.png)

### Combat / Turn Order

![Combat screenshot](screenshots/combat.png)

### Inventory

![Inventory screenshot](screenshots/inventory.png)

---

## 1. System Overview

Chrono Rift runs as three separate OS processes sharing one System V shared memory segment. No pipes carry combat data.

- **Arbiter** — core process. Allocates shared memory, sets up all mutexes and POSIX semaphores, forks HIP and ASP, then runs the main game loop. Owns all state updates.
- **HIP** — reads keyboard input and drives the SFML window. One pthread per hero.
- **ASP** — runs one pthread per enemy slot. All entity data (health, stamina, turn order, inventory, artifact ownership) lives in the shared segment.

---

## 2. Input Path

When a player's turn starts, the arbiter posts `toPlayer[idx]`. HIP keeps one pthread per hero — each thread blocks on its semaphore until the arbiter wakes it.

Only the thread whose index matches `currentActorId` reads keyboard input. All others go back to sleep. The active thread writes the chosen action into `pendingAction` and posts `humanReady`. The arbiter picks it up, acquires the locks, applies the action, and moves on.

HIP threads do not write HP or stamina. All state changes go through the arbiter.

---

## 3. NPC Path

ASP mirrors the HIP design. It spawns one pthread per enemy slot at startup. Each thread blocks on `toNpc[slot]`. When the arbiter signals a turn, the thread checks its slot against `currentActorId`, picks an action, writes to the shared buffer, and posts `aiReady`.

The arbiter enforces a **three-second timeout** per NPC turn. If `aiReady` is not posted in time, the arbiter records a skip and moves to the next actor. A single stalled thread cannot block the game.

---

## 4. Stamina-Based Scheduling

Turn order is not round-robin. A background ticker thread increments every entity's stamina at a fixed rate. An entity gets a turn only when its stamina reaches maximum.

- Most actions drain stamina to zero after use.
- Skip sets stamina to 50% of maximum (penalty for stalling).
- When multiple entities are simultaneously at full stamina, the arbiter reads their `readyAtMono` timestamps — the entity that has been ready longest goes first. Ties resolve by lower entity ID.

Ordering is always deterministic. One action runs at a time.

---

## 5. Turnaround Time Analysis (`std::chrono::steady_clock`)

| Metric | Count | Min (ms) | Avg (ms) | Max (ms) |
|---|---|---|---|---|
| Player turn end-to-end | 28 | 265.474 | 1197.057 | 4284.855 |
| Player arbiter apply | 28 | 0.005 | 0.009 | 0.022 |
| NPC turn end-to-end | 39 | 0.020 | 0.238 | 3.497 |
| NPC arbiter apply | 39 | 0.000 | 0.004 | 0.008 |

**Player turns (28 samples):** End-to-end averaged 1197 ms, peaked at 4285 ms. The timer starts when the arbiter hands the turn to HIP and stops after the action is applied — most of that time is human think time. Arbiter apply averaged **0.009 ms** (9 µs). The rules check and shared memory write are not the bottleneck.

**NPC turns (39 samples):** More NPC turns than player turns — multiple enemies refill stamina faster and stack up turns. End-to-end averaged **0.238 ms**, worst case 3.497 ms. No NPC hit the three-second timeout. NPC arbiter apply averaged **4 µs**.

The arbiter does very little work per turn. The only real source of latency is the player.

---

## 6. Signal Handling

Signals handle asynchronous events between processes, removing the need for polling.

| Signal | Sender | Effect |
|---|---|---|
| `SIGUSR1` | Arbiter | Stuns a HIP or ASP thread for the stun duration |
| `SIGSTOP` | Arbiter | Freezes all ASP threads (ultimate ability) |
| `SIGCONT` | Arbiter | Resumes ASP after ultimate ends |
| `SIGALRM` | Arbiter (internal) | Tracks the ten-second ultimate timer |
| `SIGTERM` | HIP | Triggers arbiter cleanup on player quit |

---

## 7. Artifacts and Deadlock Prevention

Solar Core and Lunar Blade are single-instance. A shared table (protected by a dedicated mutex) tracks `exists` and `holderId` per artifact.

Before any entity uses an artifact, `tryAcquireArtifactForEntity` checks the table. If another entity holds it, the request fails and the turn proceeds without it.

A **watchdog thread** inside the arbiter scans the `waitingArtifact` chain on a fixed interval. If it finds a cycle (e.g. entity A holds Solar Core and waits for Lunar Blade while entity B does the reverse), it forces a release from one holder. The cycle breaks and the game continues.

---

## 8. Inventory and Memory Management

Each hero has a primary inventory with **20 slots**. Multi-tier weapons require contiguous space and cannot be split across non-adjacent positions.

When a hero picks up a weapon with no room, `placeWeaponFit` evicts existing items to long-term storage to open a contiguous gap. Swapping a weapon back in costs the hero their full turn and the weapon cannot fire until the next turn grant. Poor inventory planning has a real, measurable cost.

---

## 9. RNG Seed and HP Formulas

RNG seed for this submission: **643** (from roll number 24i-0643).

```bash
export CHRONO_ROLL=643
./arbiter_exec
```

| Value | Formula |
|---|---|
| Hero max HP | roll + uniform int in [100, 1000] |
| Enemy max HP | last two digits of roll + uniform int in [50, 200] |
| Player damage | last decimal digit of roll + 10 |
| Enemy damage | tens digit of roll + 10 |

All results are reproducible with the seed set.

---

## 10. Docker Configuration

Build and link inside the course Docker image. SFML requires a display server — follow the README for X11 forwarding on your host.

```bash
export LIBGL_ALWAYS_SOFTWARE=1   # force CPU-based OpenGL on machines without GPU
export CHRONO_VSYNC=0            # suppress VSync warnings on software renderers
./arbiter_exec
```

---

## 11. Multiplayer Design

Local multiplayer uses one HIP process with up to **four player pthreads**. Each thread handles hot-seat input for one hero. Rendering stays in one SFML window and the shared memory layout stays simple.

This is a deliberate architectural choice — a single HIP process keeps rendering centralized and avoids synchronization overhead from two separate processes competing to draw the same window.
