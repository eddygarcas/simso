# simso — Operating System Simulator

A small terminal simulation of a mono‑tasking operating system: a round‑robin
CPU scheduler with Banker's‑style deadlock avoidance, a fixed‑size memory
allocator, and separate I/O and resource‑wait queues. It runs entirely in the
terminal as a live ncurses dashboard.

The program began life in the 1990s as a university project written for
MS‑DOS / Turbo‑C, using the BGI graphics library (`libregra.h`) and a mouse
library (`raton.h`) for its interface. This version is a port of that original
to a modern Linux terminal: the scheduling logic is preserved, while the
graphics/mouse/DOS layer has been replaced with ncurses and keyboard input.

## What it does

Every tick, the simulator schedules processes through their full lifecycle and
draws the current system state:

- A **memory bar** of 64 cells, coloured per owning process.
- **Available resources** (three pools: A, B, C).
- Four queues: **Waiting queue**, **Ready**, **I/O queue**, and the
  **resource‑wait queue** (Resources).
- A **PCB panel** for the process currently on the CPU — state, priority,
  remaining execution time, maximum/assigned resources, and I/O bursts.

Processes request and release resources; the Banker's‑style check grants a
request only if it keeps the system in a safe state, otherwise the process is
parked on the resource‑wait queue until resources free up. Processes also
periodically block for I/O and are re‑admitted from the waiting queue as memory
and scheduler slots become available.

## Requirements

- Linux on x86‑64 (or any POSIX system with ncurses).
- A C compiler — GCC/Clang, **or [Fil-C](https://fil-c.org)** (`filcc`).
- ncurses development headers (`libncurses-dev` on Debian/Ubuntu). Not needed
  for the Fil-C build — see below.
- A terminal at least **80×24**; the program refuses to start on anything
  smaller.

## Build

### With GCC (or Clang)

```sh
# install ncurses headers once (Debian/Ubuntu)
sudo apt-get install libncurses-dev

# standard build
gcc -O2 -Wall simso.c -o simso -lncurses
```

For a smaller release binary, drop debug info and strip symbols:

```sh
gcc -O2 -s simso.c -o simso -lncurses
```

Fancier flags (`-O3`, `-flto`, `--gc-sections`, `-Os`) don't help here: the
program is tiny and spends nearly all its wall‑time asleep between animation
frames, so `-O2` is the sweet spot.

Note: GCC's `-O2` flow analysis emits a few `-Warray-bounds` warnings on the
early‑exit idiom in `evitacion()`/`recursos()`. They are false positives — the
index stays in range at runtime, confirmed under AddressSanitizer/UBSan.

### With Fil-C

[Fil-C](https://fil-c.org) is a memory‑safe C/C++ implementation: it enforces
bounds and use‑after‑free checks at runtime and aborts with a
`filc panic` on any violation. It compiles this project unchanged:

```sh
filcc -O2 -g -o simso simso.c -lncurses
```

- Fil-C ships a pizlonated **ncurses** (6.5) in its `pizfix`, so no extra
  library setup is needed — the `-lncurses` above resolves against it
  automatically.
- Keep **`-g`** while developing: Fil-C's panic backtraces are far more useful
  with debug info (you get line numbers pointing at the offending access).

**Troubleshooting:** if the link fails with a wall of
`undefined reference to 'pizlonated_stdscr'`, `'pizlonated_initscr'`, etc., you
forgot `-lncurses` — those are the ncurses symbols with nothing to resolve
against. If `-lncurses` isn't found, try `-lncursesw`, or check what your
pizfix provides (e.g. `ls /opt/fil/*/lib | grep -i ncurses`).

## Run

```sh
./simso
```

On start you're prompted for:

1. **Quantum** — CPU time slice (1–50).
2. **I/O time** — how long an I/O burst blocks a process (1–50).
3. **Finish mode:**
   - `1` — stop after a chosen number **N** of processes complete.
   - `2` — run until all processes have finished.

Press **`q`** at any time during the simulation to abort cleanly. The terminal
is always restored on exit.

At runtime ncurses needs `$TERM` set and a matching terminfo entry; the system
default (`/usr/share/terminfo`) is fine.

> **Known quirk (preserved from the original):** finish mode `2` exits almost
> immediately, after the first quantum. The original code uses the value `0`
> both to mean "run until all done" and as the finite‑mode countdown, so its
> `numero == 0` exit test fires right away. This behavior was kept faithful to
> the source. To make mode 2 run to completion, capture the mode in a separate
> flag and gate that check on it.

## Porting & hardening notes

The scheduling core is logically identical to the DOS original. Two kinds of
change were made:

**Interface (the point of the port).** BGI graphics calls became an ncurses
dashboard; the mouse + checkbox setup dialog became keyboard prompts;
`conio.h`/`dos.h` calls (`clrscr`, `gotoxy`, `delay`, `randomize`) became their
ncurses/stdlib equivalents. `libregra.h` and `raton.h` are gone entirely. All
on‑screen text was also translated from Catalan/Spanish to English.

**Memory‑safety fixes.** DOS tolerated several out‑of‑bounds accesses that
happened to overlap adjacent memory harmlessly; on Linux (and under Fil-C)
those are crashes or undefined behavior. Each fix is commented inline in the
source and is small and reversible:

- Writes past the 64‑cell `memoria[]` array clamped to valid bounds.
- Out‑of‑bounds `PCB[12]` reads in the resource‑search loops removed by
  bounding the search and ordering the `&&` guard so it short‑circuits.
- The I/O‑burst array `tiempoes[]` was one slot too small for the index range
  the code uses; enlarged and fully initialised.
- The original `rand() % rand()` (which can divide by zero) replaced with an
  equivalent guarded helper.
- Free‑cell counters reset between passes to avoid a stale‑index read.

After these changes the program is clean across repeated AddressSanitizer +
UBSan runs and both finish‑modes exit with status 0. Running it under Fil-C is
a good ongoing check: the original would almost certainly have tripped Fil-C's
guard on the first run.

A handful of unbounded sentinel searches (the `!= 'z'` scans) were left as‑is;
they rely on invariants that held throughout testing. If Fil-C ever panics in
one of those, that's a genuine bug worth investigating rather than a false
positive.

## Project layout

Single translation unit:

- `simso.c` — everything (presentation layer + scheduler core), with a header
  comment on each function.

## Credits

Original DOS/Turbo‑C project by **Eduard G.Castello and Llorenç Llado**.
Linux/ncurses port and hardening applied to the original source.