bsnes-oracle
============

A fork of [bsnes](https://github.com/bsnes-emu/bsnes) v115 with a new
headless **debugger-oracle** target — a console driver built for
reverse-engineering and engine-reconstruction work.

It exists to answer one question reliably: *what does the bsnes accuracy
core do?* The oracle runs a ROM headless and dumps reference state —
WRAM/VRAM/CGRAM/APU memory, PPU state, a per-byte VRAM write journal —
so a re-implementation can be diffed against the emulator's behavior,
byte for byte.

Built for the [Evermore Project](https://evermore.ancient.monster/), a
reconstruction of Secret of Evermore's engine in Godot.

What the oracle target adds
---------------------------

- **Breakpoints and watchpoints** on the 65816 bus and the SPC700 APU
  (`--exec-bp`, `--spc-bp`, `--watch-r/w`, `--reset-bp`), with
  single-stepping (`--steps`) and scripted pad input per frame
  (`--press` / `--release F:BTN`).
- **A passive SPC700 write watch** (`--spc-watch-w ADDR|LO-HI`, repeatable)
  that logs every real APU RAM store into a range — frame, writing SPC PC,
  address, value — without ever halting the machine, so armed watches can't
  reorder scheduler events. `--watch-from F` drops hits before frame F;
  `--watch-max-print N` caps the log (excess hits are still counted and
  reported in the exit summary).
- **State dumps at exit, or at a chosen frame** (`--snapshot-at`):
  VRAM (128 KiB), CGRAM, PPU background state, extended PPU registers,
  APU RAM (64 KiB) and the DSP register file. WRAM dumps separately at
  exit, or at the first break hit (`--wram-at-hit`).
- **A VRAM write journal** — every byte written to VRAM, with the
  address, byte lane, and whether the CPU or DMA wrote it. Indispensable
  for figuring out how a frame is actually composed.
- **Deterministic re-runs** via save/load state and fixed input
  schedules, so two runs of the same experiment produce identical
  output.
- **An interactive JSON channel** (`--interactive`) over stdin/stdout:
  `run_to`, `step`, `bp_add/del`, `regs`, `mem_read` and friends — so
  scripts and agents can drive the emulator like a debugger.

Upstream behavior is unchanged: the fork adds read-only, inert-when-unused
hooks (~200 lines) inside sfc/ and a new build target; everything else is
stock v115. Note: PPU/VRAM/CGRAM dumps read the accuracy renderer — pass
`--accurate` (the fast renderer keeps its own copies, so dumps in fast
mode are not meaningful).

How this differs from existing debugger emulators
--------------------------------------------------

[Mesen2](https://github.com/SourMesen/Mesen2) has an excellent debugger
UI and a Lua-scriptable test runner. [BizHawk](https://github.com/TASEmulators/BizHawk)
scripts well too (its SNES core is a bsnes port). [bsnes-plus](https://github.com/devinacker/bsnes-plus)
is a GUI debugger fork of an older bsnes. This target is a different
shape, not a better one: a small headless bsnes v115 frontend whose
entire interface is NDJSON over stdio and raw binary dumps to files —
trivially spawnable from scripts, CI, or AI agents, with deterministic
re-runs. If you want an interactive debugger, use Mesen2. If you want an
emulator as a subprocess instrument on the v115 core, this exists.

Use cases
---------

- **Engine reconstruction / decompilation** — a re-implementation (in
  any language) can be diffed against the emulator's reference state,
  byte for byte. This is what it was built for.
- **Automated regression testing of ROM hacks** — run a hack to a
  breakpoint or frame budget, hash the state dumps, compare against a
  known-good baseline. Deterministic re-runs make the comparison sound.
- **Agentic / scripted reverse engineering** — the `--interactive` JSON
  channel (run_to, step, bp_add, regs, mem_read, …) turns the emulator
  into a tool an AI agent can drive like a debugger: form a hypothesis,
  set a breakpoint, inspect state, repeat. GUI debuggers assume a human
  in the loop; this one doesn't need one.
- **Tool-assisted research** — frame-indexed input schedules and
  save/load state support scripted, reproducible experiments without a
  TAS editor.

Status
------

Working and in daily use as a RE reference. The first five reported
issues (journal default arming, LoROM reset vector, interactive CLI
breakpoints, dump-failure exit codes, step(n) completion) are fixed and
verified; remaining known limits: interactive save/load restores
emulator state but not the host-side frame/input bookkeeping, and the
oracle makefile's no-OpenMP defaults apply too late to strip the flags.
Fresh batch runs with fixed input are deterministic.

Branches
--------

- **`v115`** (this branch) — the oracle on the original bsnes v115 base.
- **`main`** — the oracle rebased onto current upstream bsnes `master`
  (`78474e5d3`, 2026-10-07).

The two are **not result-compatible**. Upstream reworked SNES auto-joypad
polling after v115 (timing, plus an `HVBJOY` report bit) and bumped the
save-state serializer version. On the Secret of Evermore boot with an
`--exec-bp 0x8CA960` run, `v115` hits at frame 3115 (WRAM md5
`9d5fba13bf8765b6f8b14c299b30d902`) and `main` at frame 3108 (md5
`e285bff86ffa646b621624402bcff928`). Reverting the auto-joypad commits on
`main` reproduces the `v115` result exactly. Both are deterministic across
re-runs; save states do not load across the two. Pick one line per project
and keep its recorded baselines with it.

Building
--------

```sh
make -C bsnes target=oracle -j$(nproc)   # → bsnes/out/bsnes-oracle
```

No GUI dependencies; the driver is a plain console binary. Run it with
no arguments for the full option list.

License
-------

GPLv3, as bsnes. See `GPLv3.txt`. bsnes is by byuu and contributors —
see `CREDITS.md` in the upstream repository.
