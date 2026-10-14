bsnes-oracle
============

A fork of [bsnes](https://github.com/bsnes-emu/bsnes) v115 with a new
headless **debugger-oracle** target — a console driver built for
reverse-engineering and engine-reconstruction work.

It exists to answer one question reliably: *what does the real hardware
pipeline actually do?* The oracle runs a stock ROM under the bsnes
accuracy core and dumps ground truth — instruction traces, memory
accesses, VRAM/CGRAM/WRAM/APU state — so a re-implementation can be
diffed against it, byte for byte.

Built for the [Evermore Project](https://evermore.ancient.monster/), a
reconstruction of Secret of Evermore's engine in Godot.

What the oracle target adds
---------------------------

- **Breakpoints and watchpoints** on the 65816 bus and the SPC700 APU
  (`--exec-bp`, `--spc-bp`, `--watch-r/w`, `--reset-bp`), with
  single-stepping (`--steps`) and scripted pad input per frame
  (`--press` / `--release F:BTN`).
- **State dumps at exit or at a chosen frame** (`--snapshot-at`):
  WRAM (128 KiB), VRAM (128 KiB), CGRAM, PPU background state, extended
  PPU registers, APU RAM (64 KiB) and the DSP register file. WRAM can
  additionally be dumped at the first break hit (`--wram-at-hit`).
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
stock v115.

Why not an existing debugger emulator?
--------------------------------------

Fair question — the SNES scene has good ones. They just optimize for a
human at the keyboard, not a program:

- **[Mesen2](https://github.com/SourMesen/Mesen2)** — probably the best
  SNES debugger UI there is. But it's GUI-first; its automation is a
  test-runner mode for pass/fail test ROMs, not a scriptable debug
  session with dumps and scripted input.
- **[BizHawk](https://github.com/TASEmulators/BizHawk)** — genuinely
  scriptable via Lua, but Windows-first, and its SNES core is itself a
  port of bsnes. Same accuracy with more layers.
- **[bsnes-plus](https://github.com/devinacker/bsnes-plus)** — a
  debugger fork of bsnes, closest in spirit, but a GUI debugger forked
  from the older bsnes lineage.
- **no$sns** — a great debugger, closed-source Windows software.

What was missing is an emulator as a *programmable instrument*: a
headless process you drive from scripts or agents, that runs
deterministically, takes frame-indexed input, dumps raw hardware state
to files, and re-runs byte-identically. That's the niche this target
fills — on top of the accuracy core the scene already treats as its
reference.

Use cases
---------

- **Engine reconstruction / decompilation** — a re-implementation (in
  any language) can be diffed against emulator ground truth, byte for
  byte. This is what it was built for.
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
