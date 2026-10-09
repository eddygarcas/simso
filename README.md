# simso — Operating System Simulator

A terminal simulation of a multiprogrammed operating system: a round‑robin CPU
scheduler with resource‑allocation deadlock avoidance, a fixed‑size memory
allocator, and separate I/O and resource‑wait queues. It runs as a live
ncurses dashboard, or headless across every core you have.

The program began life in the 1990s as a university project for MS‑DOS /
Turbo‑C, using the BGI graphics library (`libregra.h`) and a mouse library
(`raton.h`). It was first ported to Linux as a single file, `simso.c`, with
the DOS graphics layer swapped for ncurses and the scheduling logic preserved
verbatim.

This version refactors that port into a modular, reentrant, concurrent
program. `simso.c` is kept, unchanged, as the reference implementation.

## Layout

```
build.sh               builds the legacy simso.c with Fil-C
build-filc.sh          builds the modular src/ tree with Fil-C
Makefile               builds the modular src/ tree with the system compiler
src/sim.h  sim.c      scheduler core — no I/O, no globals, fully reentrant
src/rng.h              per-instance PRNG (xorshift64*), so runs replay exactly
src/shared.h shared.c  the sim-thread ↔ UI-thread handshake
src/ui.h   ui.c        ncurses dashboard (renderer + input)
src/bench.h bench.c    parallel Monte-Carlo sweep
src/main.c             argument parsing and thread orchestration
tests/check.c          invariant sweep over seeds and parameters
simso.c                the original single-file port, kept for reference
```

The split is the point of the refactor. In `simso.c` the scheduler and the
renderer were the same code: `round_robin()` called `imprimir_colas()`, which
drew a frame, slept 110 ms and polled the keyboard. Every piece of state was a
global. That had three consequences — the simulation could not run at any
speed other than the animation's, input was only sampled wherever the
scheduler happened to draw, and the process could not hold two simulations at
once.

`sim.c` now knows nothing about ncurses. Where the original drew a frame, the
engine calls an observer callback and lets the caller decide what that means.
Everything else follows from that one change.

## Concurrency

Interactive mode runs two threads:

| thread | role |
| --- | --- |
| **sim** | runs `sim_run()`. At each former `imprimir_colas()` point it copies its state into a shared snapshot, bumps a sequence number, and parks itself for the configured step delay — or indefinitely while paused. |
| **UI** (main) | wakes on its own ~60 Hz timer, takes the latest snapshot under the lock, releases the lock, and draws. Input is read every frame. |

The mutex is held for two struct copies and nothing else; all drawing and all
sleeping happen outside it. Because input no longer depends on the scheduler
reaching a draw call, pause, single‑step, speed control and quit respond
immediately even while the engine is parked.

## Parallelism

`--bench N` runs N independent simulations across worker threads. Each run is a
closed system with its own `sim_t` and its own `rng_t`, so nothing is shared
except the few microseconds of handing out the next run index.

```
threads   wall (200k runs)   runs/s     speedup
      1             3.509s    57,002       1.0×
      2             1.770s   113,012       2.0×
      4             1.002s   199,691       3.5×
      8             0.590s   338,889       5.9×
     16             0.488s   410,016       7.2×
```

(16 logical cores, 8 physical.)

This is what the refactor bought: the original could not have run two
simulations in one process at all. It also makes the simulator useful for
something the animated view cannot do — the scheduler is stochastic, so a
single run tells you very little, while a few hundred thousand tell you the
deadlock rate and how a parameter change actually moves it.

## Build

### With the system compiler

```sh
make            # release build -> ./simso
make debug      # -O0 -g3
make asan       # AddressSanitizer + UBSan
make check      # invariant sweep (see Testing)
make bench      # 500-run parallel sweep
```

Requires ncurses with wide‑character support (`libncursesw-dev` on
Debian/Ubuntu) and pthreads. The build is warning‑clean under
`-Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes -Wmissing-prototypes`.

### With Fil-C

[Fil-C](https://fil-c.org) is a memory‑safe C implementation: it enforces
bounds and use‑after‑free checks at runtime and aborts with a `filc panic`
rather than corrupting memory. Running this simulator under it is a standing
check on the hardening work — the DOS original would almost certainly have
tripped it on the first run.

```sh
./build-filc.sh                  # -O2 -g, keeps Fil-C's panic backtraces useful
./build-filc.sh --debug          # -O0 -g3
./build-filc.sh --release        # -O2, stripped (no backtraces)
./build-filc.sh -o simso-filc    # choose the output name
./build-filc.sh --clean
CC=gcc ./build-filc.sh           # same script, ordinary compiler
```

`make filc` is a shortcut for `./build-filc.sh`.

#### What the script does beyond `filcc -O2 src/*.c`

- **Finds `filcc`** on `PATH` or in the usual install directories, and fails
  with install instructions rather than quietly falling back to an ordinary
  compiler — a silent fallback hands you a binary you *believe* is
  memory‑checked but isn't.
- **Probes which ncurses the toolchain actually has.** The dashboard draws
  multi‑byte Unicode block glyphs, and only the wide‑character build
  (`ncursesw`) measures their column width correctly; a narrow build renders
  each glyph as several garbled columns. If a pizfix ships only the narrow
  `ncurses`, the script defines `SIMSO_FORCE_ASCII` and the display falls
  back to ASCII glyphs at build time instead of breaking at run time.
- **Probes warning flags one at a time**, because Fil-C's frontend does not
  accept every GCC warning switch, and a rejected one would otherwise fail
  the whole build.
- **Rebuilds incrementally**, tracking both `.c` and `.h` timestamps, and
  **stamps objects with the compiler and flags** they were built with — so
  switching between `--debug` and `--release` forces a full rebuild instead
  of silently relinking the other variant's objects.

Keep `-g` (the default) while developing — Fil-C's panic backtraces are far
more useful with debug info. To exercise the scheduler hard without tying up
a terminal:

```sh
./simso --bench 5000 --all
```

#### Verification status

Fil-C was not installed on the machine this script was written on, so **the
`filcc` path itself is unproven**. Everything else was verified by driving
the identical code path with `CC=gcc`:

| checked | how |
| --- | --- |
| compiler discovery | `PATH` lookup and the install‑directory search |
| missing‑compiler error | run with no `filcc` present |
| ncurses probe | selects `-lncursesw` where both exist |
| ASCII fallback | built with `SIMSO_FORCE_ASCII`, confirmed no Unicode in the rendered frame |
| `--debug` / `--release` | 218,624 → 47,800 bytes stripped |
| `--clean`, `--help`, `-o` | each exercised |
| bad option, missing `-o` argument | both exit non‑zero with a message |
| incremental rebuild | `.c` edit rebuilds one object; `.h` edit rebuilds all |
| flag invalidation | `--release` after `--debug` rebuilds rather than relinking |

What remains untested is whether `filcc` accepts these exact flags. If it
rejects one, the script reports the file and the compiler's own error rather
than failing opaquely.

### A note on output names

`build.sh`, `build-filc.sh` and `make` all write `./simso` by default, but
`build.sh` builds the **legacy** `simso.c` while the other two build `src/`.
Running `build.sh` after either of the others replaces the modular binary
with the single‑file one. Use `build-filc.sh -o <name>` if you want both
side by side.

`./simso` is also still a tracked file in git (the old committed binary), so
every build shows up as a diff. You probably want `git rm --cached simso`
plus a `.gitignore` entry for `simso`, `build/`, `*.o` and `tests/check`.

## Run

```sh
./simso                          # prompts for quantum, I/O time, mode
./simso -q 10 -i 5 --all         # straight to a run
./simso --seed 4242 --paused     # reproducible, start paused
./simso --bench 5000 -j 8        # headless, 8 threads
./simso --bench 5000 --banker    # with real Banker's avoidance
./simso --help
```

### Keys

| key | action |
| --- | --- |
| `space` | pause / resume |
| `n` | single step |
| `+` / `-` | faster / slower |
| `0` | uncapped speed |
| `1` | slow |
| `q` | quit |

### The dashboard

- **Memory** — 64 cells coloured by owning process, with occupancy and free‑region count.
- **Resources** — A/B/C pools as proportional bars, colour‑coded by pressure.
- **Processes** — every PCB: state, priority, remaining‑execution bar, memory, per‑resource holdings, pending I/O bursts, CPU ticks consumed.
- **CPU timeline** — a scrolling Gantt strip, one cell per tick, coloured by the process that held the CPU.
- **Queues** — ready, I/O, resource‑wait and waiting, with the dispatch head marked.
- **Events** — a scrolling log of admissions, dispatches, preemptions, blocks and exits.

The layout is computed per frame from the terminal size, so resizing works.
At 80×24 the timeline is dropped and the tables shrink; given more room they
come back. Non‑UTF‑8 terminals (or `--ascii`) fall back to ASCII glyphs, and
256‑colour terminals get twelve distinct process hues where 8‑colour terminals
get six plus bold.

## Testing

`make check` sweeps the engine across 3,000 seeds × 5 quanta × 4 I/O times ×
2 termination modes (120,000 runs, a few seconds) and asserts:

- no process is duplicated in a queue, or queued after terminating;
- terminated processes hold no memory and no resources;
- `available + held == total` for every resource, and never negative;
- memory occupancy stays in range;
- `all processes finished` really means every PCB terminated;
- no run stalls, and no run needs the lost‑process invariant repair.

A sample of runs is checked at **every** observer callback, not just at the
end. 600,000 runs pass; the same sweep is clean under ASan + UBSan.

This harness is how all of the bugs below were found. None of them are
reachable by watching a single animated run — that is exactly why the
headless parallel mode earns its place.

## What changed, and why

The scheduling model is the original's. Structure changed throughout; the
following **behaviour** changes are deliberate, and each is marked `CHANGE:`
at its site in `src/sim.c`.

### Bugs fixed

**Lost processes.** Two separate paths dropped a runnable process on the
floor, leaving it `PS_READY` while belonging to no queue — the dispatcher kept
counting it as runnable and kept failing to find it, and the run idled forever
with that process never finishing.

- The admission step ran between `compactar_cola_preparados()` and the
  outcome dispatch, and took the highest free ready slot — precisely the slot
  the compaction had just freed for a preempted process to return to. When
  admission won the race, the preempted process was written nowhere.
  Admission now runs after the outcome dispatch, so the slot is reserved by
  construction.
- When a process arrived from the resource queue it parked the current queue
  head with `for (i = 4; i != 0; i--)`, which stops at 1. If slot 0 was the
  only free one, the old head was parked nowhere and the next line overwrote
  it. The scan now covers slot 0.

**The ready queue could not refill itself.** Admission only ever happened
inside `round_robin()`. Once the ready queue drained while processes were
still waiting — every resident process having finished or gone to I/O —
nothing could refill it, so `round_robin()` was never reached again and the
admission step it owned never ran. The idle path now admits too.

**A dispatch state that spun without drawing.** The dispatch loop tested only
`cola_preparados[4]`. A ready queue whose head had been vacated, while slots
0–3 still held runnable processes and the resource queue could not be
satisfied, fell through the bottom of the outer loop unchanged and came back
to the identical state — forever, on a frozen screen, since that path drew
nothing. The queue is now rotated so a queued process reaches the head.

**Head‑of‑line blocking on resources.** `recursos()` retried only `res_q[0]`,
so a process whose request could never be met blocked every process behind it
indefinitely. The whole queue is now scanned; order is still respected, in
that the earliest satisfiable request wins.

**Corrupt rollback on a refused request.** `evitacion()` unwound a partial
grant with `for (k = j-1; k >= 0; k--) { asignados[j] -= solicitados[j]; … }`
— indexing by `j` inside a loop over `k`. It repeatedly adjusted the one
resource that had *not* been granted, `j` times, and left the resources that
*had* been granted applied. Every refusal corrupted `alloc[]`/`max[]`. The
rollback is now correct, and verified by the resource‑conservation invariant.

**I/O completion dropped the wrong entry.** `compactar_cola_es()` always
removed the head of the I/O queue, whichever slot had actually finished its
service. Correct only while bursts complete in strict arrival order, and
silently wrong otherwise — losing one process and servicing another twice.
Removal is now by index.

**Deadlock detection missed the interesting case.** `error_sistema()` asked
"is every queue empty except the resource queue?", which only recognises
deadlock in its most degenerate shape. It cannot see a circular wait that runs
through *memory*: processes whose I/O has finished but which cannot be swapped
back in for lack of free memory, where that memory is held by processes
blocked on resources held by the first group. Every queue is non‑empty there,
so the original reported no error and idled forever. The engine now enumerates
the ways forward instead of pattern‑matching on empty queues; the old
condition is a strict subset. Roughly 0.02% of runs are genuine deadlocks.

**Termination mode 2.** The original used `0` both to mean "run until all
done" and as the finite‑mode countdown, so its `numero == 0` test fired after
the first quantum and mode 2 exited immediately. The mode is now a separate
flag. (The previous README documented this as a known quirk; it is fixed, and
`--all` now genuinely runs to completion.)

**Unbounded scans.** The downward slot searches (`while (q[c] != 'z') c--`)
ran off the bottom of their arrays when the invariant they assumed did not
hold. All are bounded, and the not‑found case is handled.

### Deliberate changes

- **`rand()` → per‑instance xorshift64\*.** `rand()` is process‑global: two
  engines on two threads would fight over one hidden state and neither run
  would be reproducible. A `(seed, parameters)` pair now replays exactly.
- **`rand() % rand()` → uniform draws.** The original's idiom produced a
  heavily skewed distribution and could divide by zero (Borland's RNG never
  hit it by luck; glibc raises `SIGFPE`). Selections are now uniform.
- **The waiting queue is FIFO.** The original closed gaps by swapping in
  entries from the tail, silently reordering the queue, so admission order
  depended on how many gaps had opened. Compaction is now order‑preserving,
  which is what the rest of the code already assumed.
- **A real Banker's algorithm is available.** The original called itself
  "Banker's‑style" but only tested `request <= available`, which is the
  admission test, not the avoidance algorithm — it cannot prevent the deadlock
  it is named after. `--banker` adds the actual safety‑sequence check. It is
  opt‑in; the default remains faithful to the original.
- **A stall valve and an invariant repair.** Both are backstops that should
  never fire, and `make check` asserts they don't. They exist because an
  unattended batch of 200,000 runs cannot be rescued with ctrl‑C.

### Preserved on purpose

The three "empty slot" markers (`'\0'` in the waiting and I/O queues, `'z'` in
the ready and resource queues, and the `'\0'` briefly parked in `ready_q[4]`
to mean "this slot's process is on the CPU") are **not** interchangeable, and
the downward scans depend on telling them apart — they walk straight past a
`'\0'` looking for a `'z'`. Normalising them changes which slot those scans
land on, so they are kept exactly, and documented in `sim.h`.

## Credits

Original DOS/Turbo‑C project by **Eduard G. Castello and Llorenç Llado**.
Linux/ncurses port, then this modular concurrent refactor, applied to the
original source.
