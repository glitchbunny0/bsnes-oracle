//bsnes target-oracle — soe-re headless 65816 debugger oracle
//Build: make -C bsnes target=oracle    → out/bsnes-oracle
//A console driver around the bsnes v115 accuracy core with three oracle
//hooks (SuperFamicom::oracle): instruction, read, write. No GUI deps.
//
//Part of the Evermore Project tooling.

#include <sfc/sfc.hpp>

#include <heuristics/heuristics.hpp>
#include <heuristics/heuristics.cpp>
#include <heuristics/super-famicom.cpp>
#include <heuristics/game-boy.cpp>

#include <target-libretro/resources.hpp>   //Boards (boards.bml), iplrom (SMP IPL)

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <ctype.h>
#include <nall/encode/base64.hpp>
#include <nall/hash/sha256.hpp>

using namespace nall;

// ---------------------------------------------------------------------------
// driver state
// ---------------------------------------------------------------------------
static std::vector<uint32> execBreakpoints;
struct Watch { uint32 address; char kind; };  //'r' or 'w'
static std::vector<Watch> watchpoints;
struct PadEvent { uint frame; uint button; bool press; };
static std::vector<PadEvent> padScript;
static std::vector<std::pair<uint32, uint>> peeks;

static std::string romPath, wramDumpPath, cgramDumpPath, frameDumpPath, stateSavePath, stateLoadPath;
static std::string spcramDumpPath;                     //APU RAM dump
static std::string dspRegsDumpPath;                    //DSP register file dump
static uint64 maxBreakHits = 4;                        //overridable via --max-hits 
static std::string ppu2DumpPath;
static std::string vramDumpPath, ppuDumpPath, vramJournalPath;
static uint journalFromFrame = 0, journalToFrame = 0;  //0 = from power-on / no end
static std::vector<uint16> lastFrame;
static uint lastFrameW = 0, lastFrameH = 0, lastFramePitch = 0;
static bool wramAtHit = false, wramDumped = false;
//snapshot all requested state dumps at the instant frame
//`snapshotAtFrame` is PRESENTED (videoFrame callback) instead of at run
//exit — removes the capture-epoch skew between --frame and the memory
//dumps (frame-5000 renders against sprite
//state the exit-time dumps never saw). The snapshot clears each dump
//path after writing, so the exit-time dumps skip them naturally.
static uint snapshotAtFrame = 0;   //0 = off (exit-time dumps, legacy)
//defined below main()'s helpers; forward-declared for videoFrame()
static auto dumpCGRAM(const char* why) -> void;
static auto dumpVRAM(const char* why) -> void;
static auto dumpPPU(const char* why) -> void;
static auto dumpPPU2(const char* why) -> void;
static auto dumpFrame(const char* why) -> void;  //missing decl broke ANY rebuild (pre-existing at pickup)
static bool continueAfterHit = false;
static bool haveResetBP = false;
static uint32 resetBPAddress = 0;
static uint frameBudget = 3600;
static uint wallClockBudget = 240;      //seconds
static uint stepsRemaining = 0;
static bool stepArm = false;            //yield at the next instruction boundary
static bool lastDebugWasStep = false;
static uint64 breakHits = 0, stepHits = 0;
static uint frameCounter = 0;
static bool quiet = false;
static std::chrono::steady_clock::time_point tStart;

//forward-declared for videoFrame() snapshot support (same-epoch capture)
static auto dumpSPCRAM(const char* why) -> void;
static auto dumpDSPRegs(const char* why) -> void;

//virtual gamepads (Gamepad enum: Up..Start), port 1 and 2
static bool padState[2][12] = {};

static SuperFamicom::Interface* emulator = nullptr;

// ---------------------------------------------------------------------------
//interactive JSON command channel (port debug vocabulary)
// ---------------------------------------------------------------------------
//Mode flag: when set, the oracle serves the newline-delimited JSON command
//channel on stdin/stdout (responses JSON, one per line). All human-readable
//logging stays on stderr. Legacy batch flags behave exactly as before when
//the flag is clear (regression law).
static bool interactiveMode = false;
//interactive breakpoints: kind 'x' exec / 'r' read / 'w' write; keyed on the
//bus handler id + physical offset (Bus::decode) so ALL mirrors of an armed
//address hit from one bp_add (standing single-mirror law); the id separates
//same-offset targets on different devices (WRAM vs ROM etc).
struct IntBreak { char kind; uint8 id; uint32 offset; uint32 addr; };  //addr = the arming logical address (status echo)
static std::vector<IntBreak> intBreakpoints;
static uint intStepPending = 0;      //step n: armed from the host thread
static bool intHalted = true;        //boot state: halted until first run_to/step
static std::string intHaltReason = "power";  //power|run_to|step|breakpoint
static bool intHaltBreakpoint = false;
static uint32 intHaltPc = 0;
static bool intRunToActive = false;
static uint intRunToFrame = 0;
static bool intQuitRequested = false;

//decode a logical bus address to the physical target the bus maps it to.
//Called from inside the CPU coroutine (hooks) and from the host thread
//(bp_add/mem_read while halted — safe: halted means no coroutine is running).
//uses bus.decode() (the public read-only accessor) — no struct reach-ins.
//id==0 = unmapped (open bus); those cannot be breakpointed meaningfully.
static auto decodePhysical(uint32 address, uint8& id, uint32& offset) -> bool {
  SuperFamicom::bus.decode(address, id, offset);
  return id != 0;
}

// ---------------------------------------------------------------------------
// oracle hooks (called from inside the CPU coroutine)
// ---------------------------------------------------------------------------
static auto oracleInstruction(uint32 pc) -> bool {
  if(interactiveMode) {
    if(intStepPending > 0) { intStepPending--; lastDebugWasStep = true; return true; }
    //exec breakpoints keyed on (handler id, physical offset): every mirror
    //of the armed address trips this single check — the standing law: a
    //single-mirror watch reads cold while the engine fetches through another.
    uint8 id; uint32 off;
    if(decodePhysical(pc, id, off)) {
      for(auto& bp : intBreakpoints) if(bp.kind == 'x' && bp.id == id && bp.offset == off) { lastDebugWasStep = false; return true; }
    }
    return false;
  }
  if(stepArm) { stepArm = false; lastDebugWasStep = true; return true; }
  for(auto bp : execBreakpoints) if(bp == pc) { lastDebugWasStep = false; return true; }
  return false;
}
static auto oracleRead(uint32 address) -> bool {
  if(interactiveMode) {
    uint8 id; uint32 off;
    if(decodePhysical(address, id, off)) {
      for(auto& bp : intBreakpoints) if(bp.kind == 'r' && bp.id == id && bp.offset == off) { lastDebugWasStep = false; return true; }
    }
    return false;
  }
  for(auto& w : watchpoints) if(w.kind == 'r' && w.address == address) { lastDebugWasStep = false; return true; }
  return false;
}
static auto oracleWrite(uint32 address) -> bool {
  if(interactiveMode) {
    uint8 id; uint32 off;
    if(decodePhysical(address, id, off)) {
      for(auto& bp : intBreakpoints) if(bp.kind == 'w' && bp.id == id && bp.offset == off) { lastDebugWasStep = false; return true; }
    }
    return false;
  }
  for(auto& w : watchpoints) if(w.kind == 'w' && w.address == address) { lastDebugWasStep = false; return true; }
  return false;
}
//SPC700-side exec breakpoints (--spc-bp ADDR, repeatable). The SMP has
//its own PC space ($0000-$FFFF, APU RAM); no mirror bank ambiguity.
static std::vector<uint32> spcExecBreakpoints;
static bool lastDebugWasSpcStep = false;
static auto oracleSpcInstruction(uint16 pc) -> bool {
  if(stepArm) { stepArm = false; lastDebugWasSpcStep = true; return true; }
  for(auto bp : spcExecBreakpoints) if(bp == pc) { lastDebugWasSpcStep = true; return true; }
  return false;
}
static auto reportSMP(const char* tag) -> void {
  auto& r = SuperFamicom::smp.r;
  fprintf(stderr, "%s SPC_PC=%04X  YA=%04X X=%02X S=%02X P=%02X\n",
    tag, (unsigned)r.pc.w, (unsigned)r.ya.w, (unsigned)r.x, (unsigned)r.s, (unsigned)r.p);
}
static auto reportSpcContext() -> void {
  auto& r = SuperFamicom::smp.r;
  string line = SuperFamicom::smp.disassemble(r.pc.w, r.p.p);
  fprintf(stderr, "  spc disasm: %s\n", line.data());
  fprintf(stderr, "  spc bytes :");
  for(uint i = 0; i < 8; i++) fprintf(stderr, " %02X", SuperFamicom::dsp.apuramByte(r.pc.w + i));
  fprintf(stderr, "\n");
}

// ---------------------------------------------------------------------------
// platform
// ---------------------------------------------------------------------------
struct Program : Emulator::Platform {
  string baseName;

  struct Game {
    string location;
    string manifest;
    string title;
    string region;
    Markup::Node document;
    vector<uint8> program;
    vector<uint8> data;
    vector<uint8> expansion;
  } superFamicom;

  vector<uint8> saveRAM;
  bool saveRAMDirty = false;
  //save.ram served read-only from the on-disk .srm next to the ROM (optional);
  //flushed back there at exit ONLY with --srm (default off: keeps oracle runs
  //deterministic — no SRAM carryover between sessions).
  bool flushSaveRAM = false;

  Program() { platform = this; }  //#define platform in sfc.hpp expands to Emulator::platform

  auto saveRAMSize() -> uint {
    if(auto memory = superFamicom.document["board"]["memory(type=RAM,content=Save)"]) {
      if(auto size = memory["size"].text()) return strtoul(size, nullptr, 0);
    }
    return 0;
  }

  auto open(uint id, string name, vfs::file::mode mode, bool required) -> shared_pointer<vfs::file> override {
    if(name == "ipl.rom" && mode == vfs::file::mode::read) {
      return vfs::memory::file::open(iplrom, sizeof(iplrom));
    }
    if(name == "boards.bml" && mode == vfs::file::mode::read) {
      return vfs::memory::file::open(Boards, sizeof(Boards));
    }

    if(id == SuperFamicom::ID::SuperFamicom) {
      if(name == "manifest.bml" && mode == vfs::file::mode::read) {
        return vfs::memory::file::open(superFamicom.manifest.data<uint8_t>(), superFamicom.manifest.size());
      }
      if(name == "program.rom" && mode == vfs::file::mode::read) {
        return vfs::memory::file::open(superFamicom.program.data(), superFamicom.program.size());
      }
      if(name == "data.rom" && mode == vfs::file::mode::read) {
        return vfs::memory::file::open(superFamicom.data.data(), superFamicom.data.size());
      }
      if(name == "expansion.rom" && mode == vfs::file::mode::read) {
        return vfs::memory::file::open(superFamicom.expansion.data(), superFamicom.expansion.size());
      }
      if(name == "save.ram") {
        if(mode == vfs::file::mode::read) {
          if(!saveRAM.size()) return {};  //no .srm on disk: cartridge fills with 0xff
          return vfs::memory::file::open(saveRAM.data(), saveRAM.size());
        }
        //write/modify: buffered; flushed to disk next to the ROM only with --srm
        saveRAMDirty = true;
        return vfs::memory::file::open(saveRAM.data(), saveRAM.size());
      }
    }

    if(required || !name.beginsWith("msu1/")) {
      if(!quiet) fprintf(stderr, "[oracle] unhandled platform->open(id=%u, name=%s, mode=%d, required=%d)\n", id, name.data(), (int)mode, (int)required);
    }
    return {};
  }

  auto load(uint id, string name, string type, vector<string> options) -> Load override {
    if(id == SuperFamicom::ID::SuperFamicom) {
      if(loadSuperFamicom(superFamicom.location)) return {id, superFamicom.region};
      return {};
    }
    return {id, options(0)};
  }

  auto videoFrame(const uint16* data, uint pitch, uint width, uint height, uint line) -> void override {
    frameCounter++;
    //soe-re oracle: keep the latest frame (RGB555 words, pitch in BYTES)
    //interactive mode always snapshots (the `frame` command serves it)
    if((!frameDumpPath.empty() || interactiveMode) && data) {
      lastFrame.assign(data, data + (pitch / 2) * height);
      lastFrameW = width; lastFrameH = height; lastFramePitch = pitch;
    }
    if(snapshotAtFrame && frameCounter == snapshotAtFrame) {
      //dump every requested artifact NOW — same emulation epoch, INCLUDING
      //the frame image itself (the exit-time dumpFrame lands on whichever
      //frame was presented last, which is a threading race vs the budget
      //check; that one-frame skew was the entire KAT residual). Clearing
      //the paths makes the exit-time dumps skip these files.
      if(!cgramDumpPath.empty()) { dumpCGRAM("snapshot"); cgramDumpPath.clear(); }
      if(!vramDumpPath.empty()) { dumpVRAM("snapshot"); vramDumpPath.clear(); }
      if(!ppuDumpPath.empty()) { dumpPPU("snapshot"); ppuDumpPath.clear(); }
      if(!ppu2DumpPath.empty()) { dumpPPU2("snapshot"); ppu2DumpPath.clear(); }
      if(!spcramDumpPath.empty()) { dumpSPCRAM("snapshot"); spcramDumpPath.clear(); }
      if(!dspRegsDumpPath.empty()) { dumpDSPRegs("snapshot"); dspRegsDumpPath.clear(); }
      if(!frameDumpPath.empty()) { dumpFrame("snapshot"); frameDumpPath.clear(); }
    }
    //apply any pad script entries scheduled for the frame just completed
    for(auto& e : padScript) {
      if(e.frame == frameCounter) {
        padState[0][e.button] = e.press;
        if(!quiet) fprintf(stderr, "[oracle] frame %u: pad1 %s %s\n", frameCounter, buttonName(e.button), e.press ? "PRESS" : "release");
      }
    }
    //note: run_to is NOT handled here. videoFrame runs on the HOST thread
    //(System::run() calls frameEvent()->ppu.refresh() after the CPU yielded via
    //Event::Frame), so scheduler.leave() from here would co_switch the host to
    //itself. The driver loop checks intRunToActive on Frame events instead.
  }
  auto audioFrame(const double*, uint) -> void override {}
  auto inputPoll(uint port, uint, uint input) -> int16 override {
    if(port != SuperFamicom::ID::Port::Controller1) return 0;
    if(input >= 12) return 0;
    return padState[0][input] ? 1 : 0;
  }
  auto inputRumble(uint, uint, uint, bool) -> void override {}

  auto loadSuperFamicom(string location) -> bool {
    vector<uint8> rom = file::read(location);
    if(rom.size() < 0x8000) return false;

    if((rom.size() & 0x7fff) == 512) {  //strip copier header
      memory::move(&rom[0], &rom[512], rom.size() - 512);
      rom.resize(rom.size() - 512);
    }

    auto heuristics = Heuristics::SuperFamicom(rom, location);
    superFamicom.title    = heuristics.title();
    superFamicom.region   = heuristics.videoRegion();
    superFamicom.manifest = heuristics.manifest();
    superFamicom.document = BML::unserialize(superFamicom.manifest);
    superFamicom.location = location;

    uint offset = 0;
    if(auto size = heuristics.programRomSize()) {
      superFamicom.program.resize(size);
      memory::copy(&superFamicom.program[0], &rom[offset], size);
      offset += size;
    }
    if(auto size = heuristics.dataRomSize()) {
      superFamicom.data.resize(size);
      memory::copy(&superFamicom.data[0], &rom[offset], size);
      offset += size;
    }
    if(auto size = heuristics.expansionRomSize()) {
      superFamicom.expansion.resize(size);
      memory::copy(&superFamicom.expansion[0], &rom[offset], size);
      offset += size;
    }
    return true;
  }

  static auto buttonName(uint b) -> const char* {
    static const char* names[] = {"Up","Down","Left","Right","B","A","Y","X","L","R","Select","Start"};
    return b < 12 ? names[b] : "?";
  }
};

static auto parseButton(const char* s) -> int {
  std::string n = s;
  for(uint i = 0; i < 12; i++) if(n == Program::buttonName(i)) return i;
  return -1;
}

static auto dumpWRAM(const char* why) -> void {
  FILE* f = fopen(wramDumpPath.c_str(), "wb");
  if(!f) { fprintf(stderr, "[oracle] WRAM dump FAILED to open %s\n", wramDumpPath.c_str()); return; }
  fwrite(SuperFamicom::cpu.wram, 1, sizeof(SuperFamicom::cpu.wram), f);
  fclose(f);
  wramDumped = true;
  fprintf(stderr, "[oracle] WRAM dumped 0x20000 bytes (bus $7E0000-$7FFFFF) -> %s (%s)\n", wramDumpPath.c_str(), why);
}

static auto dumpCGRAM(const char* why) -> void {
  FILE* f = fopen(cgramDumpPath.c_str(), "wb");
  if(!f) { fprintf(stderr, "[oracle] CGRAM dump FAILED to open %s\n", cgramDumpPath.c_str()); return; }
  //256 colors, BGR555, little-endian words — the PPU's own palette RAM
  uint8 buf[512];
  for(uint i = 0; i < 256; i++) {
    uint16 c = SuperFamicom::ppu.cgramColor(i);
    buf[2 * i] = c & 0xFF; buf[2 * i + 1] = c >> 8;
  }
  fwrite(buf, 1, sizeof(buf), f);
  fclose(f);
  fprintf(stderr, "[oracle] CGRAM dumped 512 bytes (256 BGR555 colors) -> %s (%s)\n", cgramDumpPath.c_str(), why);
}

//APU RAM dump — the DSP's 64K apuram, the APU's whole world (SMP
//program + data + $FFC0 IPL window + DSP regs + port files), little-endian
//irrelevant (byte array), one byte per address $0000-$FFFF.
static auto dumpSPCRAM(const char* why) -> void {
  FILE* f = fopen(spcramDumpPath.c_str(), "wb");
  if(!f) { fprintf(stderr, "[oracle] SPCRAM dump FAILED to open %s\n", spcramDumpPath.c_str()); return; }
  uint8 buf[64 * 1024];
  for(uint32 i = 0; i < 64 * 1024; i++) {
    buf[i] = SuperFamicom::dsp.apuramByte((uint16)i);
  }
  fwrite(buf, 1, sizeof(buf), f);
  fclose(f);
  fprintf(stderr, "[oracle] SPCRAM dumped 0x10000 bytes (APU RAM $0000-$FFFF) -> %s (%s)\n", spcramDumpPath.c_str(), why);
}

//DSP register file dump — the SPC_DSP's own m.regs[128]: voice regs,
//$5D BRR source directory page, $4C KON, $6C flags. Internal DSP state;
//the SMP reaches it only through $F2/$F3, it never lives in apuram.
static auto dumpDSPRegs(const char* why) -> void {
  FILE* f = fopen(dspRegsDumpPath.c_str(), "wb");
  if(!f) { fprintf(stderr, "[oracle] DSPREGS dump FAILED to open %s\n", dspRegsDumpPath.c_str()); return; }
  uint8 buf[128];
  for(uint i = 0; i < 128; i++) {
    buf[i] = SuperFamicom::dsp.dspReg((uint8)i);
  }
  fwrite(buf, 1, sizeof(buf), f);
  fclose(f);
  fprintf(stderr, "[oracle] DSPREGS dumped 128 bytes (DSP register file $00-$7F) -> %s (%s)\n", dspRegsDumpPath.c_str(), why);
}

//VRAM dump — 64K words, uint16 LE, accuracy PPU's own array
static auto dumpVRAM(const char* why) -> void {
  FILE* f = fopen(vramDumpPath.c_str(), "wb");
  if(!f) { fprintf(stderr, "[oracle] VRAM dump FAILED to open %s\n", vramDumpPath.c_str()); return; }
  uint8 buf[128 * 1024];
  for(uint i = 0; i < 64 * 1024; i++) {
    uint16 w = SuperFamicom::ppu.vramWord(i);
    buf[2 * i] = w & 0xFF; buf[2 * i + 1] = w >> 8;
  }
  fwrite(buf, 1, sizeof(buf), f);
  fclose(f);
  fprintf(stderr, "[oracle] VRAM dumped 0x20000 bytes (64K uint16 LE words) -> %s (%s)\n", vramDumpPath.c_str(), why);
}

//PPU BG-state record (Q1) — little-endian, fixed layout
//  u8  bgMode ($2105 & 7)
//  u8  reserved
//  then 4 x per-BG (BG1..BG4), 13 bytes each:
//  u16 screenAddress (VRAM word addr, reg>>2<<10)
//  u16 tiledataAddress (reg&15<<12)
//  u8  screenSize (0-3: 32x32..64x64)
//  u8  tileSize (0=8x8, 1=16x16)
//  u16 hoffset, u16 voffset
//  u8  aboveEnable, u8 belowEnable, u8 reserved
static auto dumpPPU(const char* why) -> void {
  FILE* f = fopen(ppuDumpPath.c_str(), "wb");
  if(!f) { fprintf(stderr, "[oracle] PPU dump FAILED to open %s\n", ppuDumpPath.c_str()); return; }
  uint8 hdr[2] = {(uint8)SuperFamicom::ppu.bgMode(), (uint8)SuperFamicom::ppu.bgPriorityFlag()};
  fwrite(hdr, 1, 2, f);
  for(uint id = 0; id < 4; id++) {
    auto s = SuperFamicom::ppu.bgState(id);
    uint8 rec[13];
    rec[0] = s.screenAddress & 0xFF;     rec[1] = s.screenAddress >> 8;
    rec[2] = s.tiledataAddress & 0xFF;   rec[3] = s.tiledataAddress >> 8;
    rec[4] = s.screenSize;
    rec[5] = s.tileSize;
    rec[6] = s.hoffset & 0xFF;           rec[7] = s.hoffset >> 8;
    rec[8] = s.voffset & 0xFF;           rec[9] = s.voffset >> 8;
    rec[10] = s.aboveEnable;
    rec[11] = s.belowEnable;
    rec[12] = 0;
    fwrite(rec, 1, 13, f);
  }
  fclose(f);
  fprintf(stderr, "[oracle] PPU BG state dumped (2+4*13 bytes) -> %s (%s)\n", ppuDumpPath.c_str(), why);
}

//extended PPU dump — the register file the --ppu dump lacks.
//layout: [0]=INIDISP packed (bit7 disable, bits3-0 brightness), [1..11]=obj regs,
//[12..47]=window regs, [48..60]=screen/color-math regs, then 544 bytes OAM.
static auto dumpPPU2(const char* why) -> void {
  auto& p = SuperFamicom::ppu;
  FILE* f = fopen(ppu2DumpPath.c_str(), "wb");
  if(!f) { fprintf(stderr, "[oracle] PPU2 dump FAILED to open %s\n", ppu2DumpPath.c_str()); return; }
  uint8 inidisp = p.inidispDisable() << 7 | p.inidispBrightness();
  fputc(inidisp, f);
  for(uint i = 0; i < 11; i++) fputc(p.objReg(i), f);
  for(uint i = 0; i < 36; i++) fputc(p.windowReg(i), f);
  for(uint i = 0; i < 13; i++) fputc(p.screenReg(i), f);
  for(uint a = 0; a < 544; a++) fputc(p.oamByte(a), f);
  fclose(f);
  fprintf(stderr, "[oracle] PPU2 extended dump (61+544 bytes) -> %s (%s)\n", ppu2DumpPath.c_str(), why);
}

//VRAM write journal — 4 bytes/entry: u16 vram address, u8 byteIndex(0=lo,1=hi), u8 origin(1=CPU,0=DMA)
static auto dumpVramJournal(const char* why) -> void {
  auto& j = SuperFamicom::ppu.journal;
  FILE* f = fopen(vramJournalPath.c_str(), "wb");
  if(!f) { fprintf(stderr, "[oracle] VRAM journal FAILED to open %s\n", vramJournalPath.c_str()); return; }
  uint8 rec[4];
  for(uint64 i = 0; i < j.size(); i++) {
    rec[0] = j[i].address & 0xFF; rec[1] = j[i].address >> 8;
    rec[2] = j[i].byte; rec[3] = j[i].cpu;
    fwrite(rec, 1, 4, f);
  }
  fclose(f);
  fprintf(stderr, "[oracle] VRAM journal dumped %" PRIu64 " entries -> %s (%s)\n", (uint64)j.size(), vramJournalPath.c_str(), why);
}

static auto dumpFrame(const char* why) -> void {
  if(lastFrame.empty()) { fprintf(stderr, "[oracle] frame dump skipped: no frame captured\n"); return; }
  FILE* f = fopen(frameDumpPath.c_str(), "wb");
  if(!f) { fprintf(stderr, "[oracle] frame dump FAILED to open %s\n", frameDumpPath.c_str()); return; }
  //header: magic, w, h, pitch_bytes, then w*h words (row stride pitch/2)
  uint32 hdr[4] = {0x524D4652, lastFrameW, lastFrameH, lastFramePitch};
  fwrite(hdr, 4, 4, f);
  for(uint y = 0; y < lastFrameH; y++)
    fwrite(lastFrame.data() + (size_t)y * (lastFramePitch / 2), 2, lastFrameW, f);
  fclose(f);
  fprintf(stderr, "[oracle] frame dumped %ux%u RGB555 -> %s (%s)\n", lastFrameW, lastFrameH, frameDumpPath.c_str(), why);
}

// ---------------------------------------------------------------------------
//interactive JSON command channel
// ---------------------------------------------------------------------------
// Newline-delimited JSON on stdin; one JSON response per command on stdout.
// Vocabulary (STABLE — the port debug interface shares it):
//   run_to {frame}          run until frame N presents, then halt
//   step {n?}               execute n instructions (default 1), then halt
//   bp_add {kind, addr}     arm exec|read|write bp (ALL mirrors, via bus decode)
//   bp_del {kind, addr}     disarm
//   regs {}                 CPU register snapshot
//   mem_read {addr, len}    bus read (no side effects), base64 data (cap 4096)
//   wram {}                 128 KiB WRAM dump, base64
//   vram {}                 64K-word VRAM dump, base64
//   cgram {}                512 B CGRAM dump, base64 (requires --accurate)
//   frame {path?}           current framebuffer, RFMR file or inline base64
//   state_save {path?}      serializer snapshot (default /tmp/soe-oracle path)
//   state_load {path}       restore snapshot (renderer mismatch = explicit error)
//   status {}               frame, halt reason, armed bps
//   quit                    clean shutdown (EOF behaves the same)
// Every response: {"ok":true,...} or {"ok":false,"error":"..."}; unknown
// commands are explicit errors, never silence.

static auto jsonEscape(const std::string& s) -> std::string {
  std::string r;
  for(char c : s) {
    switch(c) {
    case '"':  r += "\\\""; break;
    case '\\': r += "\\\\"; break;
    case '\n': r += "\\n"; break;
    case '\r': r += "\\r"; break;
    case '\t': r += "\\t"; break;
    default:
      if((uint8)c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); r += b; }
      else r += c;
    }
  }
  return r;
}

//minimal flat-object JSON reader for the command vocabulary: keys are known,
//values are strings or integers. Tolerant of key order and whitespace.
struct JsonCmd {
  bool ok = false;
  std::string cmd;
  bool hasFrame = false; uint32 frame = 0;
  bool hasCount = false; uint32 count = 0;
  bool hasAddr  = false; uint32 addr = 0;
  bool hasLen   = false; uint32 len = 0;
  bool hasKind  = false; std::string kind;
  bool hasPath  = false; std::string path;
};

static auto jsonSkipWs(const std::string& s, size_t& i) -> void {
  while(i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
}

static auto jsonParseString(const std::string& s, size_t& i, std::string& out) -> bool {
  if(i >= s.size() || s[i] != '"') return false;
  i++;
  out.clear();
  while(i < s.size() && s[i] != '"') {
    if(s[i] == '\\' && i + 1 < s.size()) {
      i++;
      switch(s[i]) {
      case 'n': out += '\n'; break;
      case 't': out += '\t'; break;
      case 'r': out += '\r'; break;
      default:  out += s[i]; break;  //\" \\ \/ and pass-through
      }
    } else out += s[i];
    i++;
  }
  if(i >= s.size()) return false;
  i++;  //closing quote
  return true;
}

static auto jsonParseUint(const std::string& s, size_t& i, uint32& out) -> bool {
  //JSON number, integer only; 0x prefix accepted (bus addresses are hex-speak)
  jsonSkipWs(s, i);
  uint64 v = 0; bool any = false;
  if(i + 1 < s.size() && s[i] == '0' && (s[i+1] == 'x' || s[i+1] == 'X')) {
    i += 2;
    while(i < s.size() && isxdigit((uint8)s[i])) { v = v * 16 + (isdigit((uint8)s[i]) ? s[i] - '0' : (tolower(s[i]) - 'a' + 10)); v &= 0xFFFFFFFF; i++; any = true; }
  } else {
    while(i < s.size() && isdigit((uint8)s[i])) { v = v * 10 + (s[i] - '0'); v &= 0xFFFFFFFF; i++; any = true; }
  }
  if(!any) return false;
  out = (uint32)v;
  return true;
}

static auto jsonParseCommand(const std::string& line) -> JsonCmd {
  JsonCmd c;
  size_t i = 0;
  jsonSkipWs(line, i);
  if(i >= line.size() || line[i] != '{') return c;
  i++;
  bool haveCmd = false;
  while(true) {
    jsonSkipWs(line, i);
    if(i < line.size() && line[i] == '}') { i++; c.ok = haveCmd; return c; }
    std::string key;
    if(!jsonParseString(line, i, key)) return c;
    jsonSkipWs(line, i);
    if(i >= line.size() || line[i] != ':') return c;
    i++;
    jsonSkipWs(line, i);
    if(i < line.size() && line[i] == '"') {
      std::string v;
      if(!jsonParseString(line, i, v)) return c;
      if(key == "cmd") { c.cmd = v; haveCmd = true; }
      else if(key == "kind") { c.kind = v; c.hasKind = true; }
      else if(key == "path") { c.path = v; c.hasPath = true; }
    } else {
      uint32 v;
      if(!jsonParseUint(line, i, v)) return c;
      if(key == "frame") { c.frame = v; c.hasFrame = true; }
      else if(key == "n") { c.count = v; c.hasCount = true; }
      else if(key == "addr") { c.addr = v; c.hasAddr = true; }
      else if(key == "len") { c.len = v; c.hasLen = true; }
      //unknown keys ignored (forward compatibility)
    }
    jsonSkipWs(line, i);
    if(i < line.size() && line[i] == ',') { i++; continue; }
    if(i < line.size() && line[i] == '}') { i++; c.ok = haveCmd; return c; }
    return c;
  }
}

static auto respondRaw(const std::string& json) -> void {
  fputs(json.c_str(), stdout);
  fputc('\n', stdout);
  fflush(stdout);
}

static auto respondError(const std::string& cmd, const std::string& error) -> void {
  respondRaw("{\"ok\":false,\"cmd\":\"" + jsonEscape(cmd) + "\",\"error\":\"" + jsonEscape(error) + "\"}");
}

static auto kindChar(const std::string& k) -> char {
  if(k == "exec") return 'x';
  if(k == "read") return 'r';
  if(k == "write") return 'w';
  return 0;
}
static auto kindName(char k) -> const char* {
  return k == 'x' ? "exec" : k == 'r' ? "read" : k == 'w' ? "write" : "?";
}

//register snapshot (public cpu.r — no reach-ins needed, Registers is public)
static auto respondRegs(const std::string& cmd) -> void {
  auto& r = SuperFamicom::cpu.r;
  char b[256];
  snprintf(b, sizeof b,
    "{\"ok\":true,\"cmd\":\"%s\",\"pc\":%u,\"a\":%u,\"x\":%u,\"y\":%u,\"sp\":%u,\"dp\":%u,\"db\":%u,\"p\":%u,\"e\":%d,\"mf\":%d,\"xf\":%d,\"frame\":%u}",
    jsonEscape(cmd).c_str(),
    (unsigned)r.pc.d, (unsigned)r.a.w, (unsigned)r.x.w, (unsigned)r.y.w,
    (unsigned)r.s.w, (unsigned)r.d.w, (unsigned)r.b, (unsigned)r.p,
    (int)r.e, (int)r.p.m, (int)r.p.x, frameCounter);
  respondRaw(b);
}

static auto respondStatus(const std::string& cmd) -> void {
  std::string out = "{\"ok\":true,\"cmd\":\"" + jsonEscape(cmd) + "\",\"frame\":" + std::to_string(frameCounter);
  out += ",\"halted\":" + std::string(intHalted ? "true" : "false");
  out += ",\"halt_reason\":\"" + jsonEscape(intHaltReason) + "\"";
  if(intHaltBreakpoint) out += ",\"hit_pc\":" + std::to_string(intHaltPc);
  out += ",\"bps\":[";
  for(uint i = 0; i < intBreakpoints.size(); i++) {
    auto& bp = intBreakpoints[i];
    if(i) out += ",";
    out += std::string("{\"kind\":\"") + kindName(bp.kind) + "\",\"addr\":" + std::to_string(bp.addr) + "}";
  }
  out += "],\"bps_armed\":" + std::to_string(intBreakpoints.size());
  out += "}";
  respondRaw(out);
}

static auto b64Field(const uint8_t* data, uint size) -> std::string {
  std::string r = "\"";
  r += Encode::Base64(data, size);   //nall string; append, don't operator+ with const char*
  r += "\"";
  return r;
}

// ---------------------------------------------------------------------------
static auto reportCPU(const char* tag) -> void {
  auto& r = SuperFamicom::cpu.r;
  fprintf(stderr, "%s PC=%06X  A=%04X X=%04X Y=%04X S=%04X D=%04X DB=%02X P=%02X  E=%d M=%d X=%d\n",
    tag,
    (unsigned)r.pc.d, (unsigned)r.a.w, (unsigned)r.x.w, (unsigned)r.y.w,
    (unsigned)r.s.w, (unsigned)r.d.w, (unsigned)r.b, (unsigned)r.p,
    (int)r.e, (int)r.p.m, (int)r.p.x);
}

static auto reportContext() -> void {
  auto& r = SuperFamicom::cpu.r;
  string line = SuperFamicom::cpu.disassemble(r.pc.d, r.e, r.p.m, r.p.x);
  fprintf(stderr, "  disasm: %s\n", line.data());
  fprintf(stderr, "  bytes :");
  for(uint i = 0; i < 8; i++) fprintf(stderr, " %02X", SuperFamicom::cpu.readDisassembler(r.pc.d + i));
  fprintf(stderr, "\n");
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
  if(argc < 2) {
    fprintf(stderr,
      "bsnes-oracle (bsnes v115 + soe-re target-oracle)\n"
      "usage: bsnes-oracle ROM.sfc [options]\n"
      "  --exec-bp ADDR       execution breakpoint, SNES bus address (repeatable; 0x hex)\n"
      "  --spc-bp ADDR        SPC700 exec breakpoint, APU RAM $0000-$FFFF (repeatable) \n"
      "  --watch-r ADDR       read watchpoint (repeatable)\n"
      "  --watch-w ADDR       write watchpoint (repeatable)\n"
      "  --reset-bp           convenience: exec bp on the reset vector from the ROM header\n"
      "  --frames N           frame budget (default 3600)\n"
      "  --seconds N          wall-clock budget (default 240)\n"
      "  --steps N            single-step N instructions after the first break hit\n"
      "  --wram FILE         dump WRAM ($7E0000-$7FFFFF, 128 KiB) to FILE\n"
      "  --cgram FILE        dump CGRAM (256 BGR555 colors, 512 B) to FILE\n"
      "  --vram FILE         dump VRAM (64K uint16 LE words, 128 KiB) to FILE (accuracy PPU only)\n"
      "  --ppu FILE          dump PPU BG state (bgMode + 4x13-byte BG records) to FILE\n"
      "  --vram-journal FILE log every VRAM byte-write (4 B/entry: addr, byteIdx, cpu?1:0) to FILE\n"
      "  --ppu2 FILE        extended PPU regs dump: INIDISP+OBJ+window+color-math+OAM (accuracy only)\n"
      "  --spcram FILE      dump APU RAM (64 KiB, $0000-$FFFF) to FILE — SPC700 program/data \n"
      "  --dspregs FILE     dump DSP register file (128 B, incl. $5D BRR source directory page) to FILE \n"
      "  --max-hits N       stop after N break hits with --continue (default 4)\n"
      "  --journal-from F    arm the VRAM journal at frame F (default: power-on)\n"
      "  --journal-to F      close the VRAM journal at frame F (default: run end)\n"
      "  --wram-at-hit        dump WRAM at the first break hit instead of at exit\n"
      "  --state-save FILE    save emulation state to FILE at exit\n"
      "  --state-load FILE    load emulation state before running\n"
      "  --press F:BTN        press pad1 BTN at frame F (repeatable)\n"
      "  --release F:BTN      release pad1 BTN at frame F (repeatable)\n"
      "  --peek ADDR:LEN      hexdump bus range after the run ends (repeatable)\n"
      "  --continue           keep running after a break hit (until budget)\n"
      "  --interactive        serve the newline-delimited JSON command channel\n"
      "                       on stdin/stdout (run_to/step/bp_add/bp_del/regs/mem_read/\n"
      "                       wram/vram/cgram/frame/state_save/state_load/status/quit)\n"
      "  --srm                load+flush cartridge save RAM (.srm next to the ROM); default off = deterministic\n"
      "  --entropy M          None | Low | High (default None: deterministic WRAM)\n"
      "  --accurate           use the accuracy PPU renderer (default: fast renderer)\n"
      "  --quiet              only essential output on stderr\n"
      "exit codes: 0 break hit / clean stop, 1 load error, 2 wall clock, 3 frame budget (bp cold), 4 max hits\n");
    return 1;
  }

  static bool wantSRM = false;
  romPath = argv[1];
  const char* entropy = "None";
  bool accuratePPU = false;

  for(int i = 2; i < argc; i++) {
    auto next = [&]() -> const char* { if(i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", argv[i]); exit(1); } return argv[++i]; };
    std::string a = argv[i];
    if(a == "--exec-bp") execBreakpoints.push_back((uint32)strtoul(next(), nullptr, 0));
    else if(a == "--spc-bp") spcExecBreakpoints.push_back((uint32)strtoul(next(), nullptr, 0));
    else if(a == "--watch-r") watchpoints.push_back({(uint32)strtoul(next(), nullptr, 0), 'r'});
    else if(a == "--watch-w") watchpoints.push_back({(uint32)strtoul(next(), nullptr, 0), 'w'});
    else if(a == "--reset-bp") haveResetBP = true;
    else if(a == "--frames") frameBudget = (uint)strtoul(next(), nullptr, 0);
    else if(a == "--seconds") wallClockBudget = (uint)strtoul(next(), nullptr, 0);
    else if(a == "--steps") stepsRemaining = (uint)strtoul(next(), nullptr, 0);
    else if(a == "--wram") wramDumpPath = next();
    else if(a == "--wram-at-hit") wramAtHit = true;
    else if(a == "--state-save") stateSavePath = next();
    else if(a == "--cgram") cgramDumpPath = next();
    else if(a == "--frame") frameDumpPath = next();
    else if(a == "--vram") vramDumpPath = next();
    else if(a == "--ppu") ppuDumpPath = next();
    else if(a == "--vram-journal") vramJournalPath = next();
    else if(a == "--ppu2") ppu2DumpPath = next();
    else if(a == "--spcram") spcramDumpPath = next();
    else if(a == "--dspregs") dspRegsDumpPath = next();
    else if(a == "--max-hits") maxBreakHits = (uint64)strtoul(next(), nullptr, 0);
    else if(a == "--snapshot-at") snapshotAtFrame = (uint)strtoul(next(), nullptr, 0);
    else if(a == "--journal-from") journalFromFrame = (uint)strtoul(next(), nullptr, 0);
    else if(a == "--journal-to") journalToFrame = (uint)strtoul(next(), nullptr, 0);
    else if(a == "--state-load") stateLoadPath = next();
    else if(a == "--press" || a == "--release") {
      std::string v = next();
      auto col = v.find(':');
      if(col == std::string::npos) { fprintf(stderr, "bad %s (want FRAME:BTN)\n", a.c_str()); return 1; }
      uint frame = (uint)strtoul(v.substr(0, col).c_str(), nullptr, 0);
      int b = parseButton(v.substr(col + 1).c_str());
      if(b < 0) { fprintf(stderr, "bad button %s\n", v.substr(col + 1).c_str()); return 1; }
      padScript.push_back({frame, (uint)b, a == "--press"});
    }
    else if(a == "--peek") {
      std::string v = next();
      auto col = v.find(':');
      if(col == std::string::npos) { fprintf(stderr, "bad --peek (want ADDR:LEN)\n"); return 1; }
      peeks.push_back({(uint32)strtoul(v.substr(0, col).c_str(), nullptr, 0),
                       (uint)strtoul(v.substr(col + 1).c_str(), nullptr, 0)});
    }
    else if(a == "--continue") continueAfterHit = true;
    else if(a == "--interactive") interactiveMode = true;
    else if(a == "--srm") wantSRM = true;
    else if(a == "--entropy") entropy = argv[++i];
    else if(a == "--accurate") accuratePPU = true;
    else if(a == "--quiet") quiet = true;
    else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 1; }
  }

  setvbuf(stderr, nullptr, _IOLBF, 0);
  tStart = std::chrono::steady_clock::now();

  Program program;
  program.flushSaveRAM = wantSRM;
  program.superFamicom.location = romPath.c_str();
  program.baseName = {Location::notsuffix(romPath.c_str()), ""};

  emulator = new SuperFamicom::Interface;
  emulator->configure("Hacks/Entropy", entropy);
  emulator->configure("Hacks/PPU/Fast", !accuratePPU);

  //--srm: load prior save RAM from next to the ROM BEFORE load (save.ram is
  //requested by the cartridge loader); round-trips with the exit flush
  if(program.flushSaveRAM) {
    string srm = {program.baseName, ".srm"};
    if(file::exists(srm)) {
      program.saveRAM = file::read(srm);
      fprintf(stderr, "[oracle] save RAM loaded from %s (%u bytes)\n", srm.data(), (unsigned)program.saveRAM.size());
    }
  }

  emulator->unload();
  if(!emulator->load()) { fprintf(stderr, "[oracle] load FAILED for %s\n", romPath.c_str()); return 1; }

  //reset-vector convenience breakpoint: file offset 0xFFFC == HiROM bank, LoROM maps $00:FFFC too
  if(haveResetBP) {
    auto& prom = program.superFamicom.program;
    if(prom.size() >= 0xFFFE) {
      resetBPAddress = prom[0xFFFC] | prom[0xFFFD] << 8;  //emulation-mode reset: bank $00
      execBreakpoints.push_back(resetBPAddress);
      fprintf(stderr, "[oracle] reset vector $%02X%02X%02X -> exec bp at $00:%04X\n",
        0, prom[0xFFFD], prom[0xFFFC], (unsigned)resetBPAddress);
    } else {
      fprintf(stderr, "[oracle] --reset-bp: program.rom too small\n");
    }
  }

  fprintf(stderr, "[oracle] loaded: %s | region=%s | %u exec bps, %u watches, %u pad events\n",
    program.superFamicom.title.size() ? program.superFamicom.title.data() : romPath.c_str(),
    program.superFamicom.region.size() ? program.superFamicom.region.data() : "?",
    (unsigned)execBreakpoints.size(), (unsigned)watchpoints.size(), (unsigned)padScript.size());
  if(!spcExecBreakpoints.empty()) {
    fprintf(stderr, "[oracle] spc exec bps: %u\n", (unsigned)spcExecBreakpoints.size());
  }

  emulator->connect(SuperFamicom::ID::Port::Controller1, SuperFamicom::ID::Device::Gamepad);
  emulator->connect(SuperFamicom::ID::Port::Controller2, SuperFamicom::ID::Device::Gamepad);
  emulator->power();

  if(!stateLoadPath.empty()) {
    auto data = file::read(stateLoadPath.c_str());
    if(!data.size()) { fprintf(stderr, "[oracle] state load FAILED: %s\n", stateLoadPath.c_str()); return 1; }
    serializer s(data.data(), data.size());
    if(!emulator->unserialize(s)) { fprintf(stderr, "[oracle] state unserialize FAILED\n"); return 1; }
    fprintf(stderr, "[oracle] state loaded from %s (%u bytes)\n", stateLoadPath.c_str(), (unsigned)data.size());
  }

  SuperFamicom::oracle.instruction = oracleInstruction;
  SuperFamicom::oracle.read = oracleRead;
  SuperFamicom::oracle.write = oracleWrite;
  SuperFamicom::oracle.spcInstruction = oracleSpcInstruction;

  //--steps arms immediately (steps from power-on or from a loaded state);
  //after a break hit, remaining steps continue from the break point.
  if(stepsRemaining > 0) stepArm = true;

  int exitCode = 0;
  bool quit = false;

  // ------------------------------------------------- interactive server
  // Newline-delimited JSON commands on stdin, responses on stdout. Halts are
  // cooperative: run_to/step arm the condition, then the run loop below
  // executes until the condition fires, and control returns HERE with the
  // machine halted. Watch/bp hits halt and REPORT; continuing is explicit
  // (another run_to/step). EOF or `quit` shuts down cleanly.
  if(interactiveMode) {
    auto runUntilHalt = [&]() -> std::string {
      //runs the emulator until a halt condition; returns the halt reason.
      //Conditions: armed step count (intStepPending), a bp/watch hit
      //(Event::Debug from the hooks), or a run_to frame arrival (Frame event).
      intHaltBreakpoint = false;
      intHaltPc = 0;
      intHalted = false;
      while(true) {
        //frame/wall budgets still bound a runaway interactive session
        std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - tStart;
        if(frameCounter >= frameBudget) { intHalted = true; intHaltReason = "frame_budget"; break; }
        if((uint)elapsed.count() >= wallClockBudget)   { intHalted = true; intHaltReason = "wall_clock";  break; }

        emulator->run();

        if(SuperFamicom::scheduler.event == SuperFamicom::Scheduler::Event::Frame) {
          //frame presented (frameCounter already bumped in videoFrame);
          //run_to target reached? (videoFrame ran: pad events applied, order
          //identical to batch mode.)
          if(intRunToActive && frameCounter >= intRunToFrame) {
            intRunToActive = false;
            intHalted = true;
            intHaltReason = "run_to";
            break;
          }
          continue;  //ordinary frame boundary: keep running
        }
        if(SuperFamicom::scheduler.event != SuperFamicom::Scheduler::Event::Debug) continue;

        if(lastDebugWasSpcStep) {
          //SPC-side events only fire from --spc-bp (interactive has no spc
          //commands); a hit halts so it is never silently swallowed.
          intHalted = true;
          intHaltReason = "spc_breakpoint";
          intHaltBreakpoint = true;
          intHaltPc = SuperFamicom::cpu.r.pc.d;
          break;
        }
        if(lastDebugWasStep) {
          intHalted = true;
          intHaltReason = "step";
          break;
        }
        //bp/watch hit
        intHalted = true;
        intHaltReason = "breakpoint";
        intHaltBreakpoint = true;
        intHaltPc = SuperFamicom::cpu.r.pc.d;
        break;
      }
      return intHaltReason;
    };

    //per-command execution: parse `line`, act, emit exactly one response.
    auto execCommand = [&](const std::string& line) -> bool {
      //returns false when the channel should close (quit/EOF)
      JsonCmd c = jsonParseCommand(line);
      if(!c.ok || c.cmd.empty()) { respondError("", "parse error: expected {\"cmd\":...} object"); return true; }
      std::string cmd = c.cmd;

      if(cmd == "quit") {
        respondRaw("{\"ok\":true,\"cmd\":\"quit\",\"bye\":true}");
        return false;
      }
      else if(cmd == "status") {
        respondStatus(cmd);
      }
      else if(cmd == "regs") {
        respondRegs(cmd);
      }
      else if(cmd == "run_to") {
        if(!c.hasFrame) { respondError(cmd, "run_to requires frame"); return true; }
        if(!intHalted) { respondError(cmd, "busy: emulation running"); return true; }
        if(c.frame <= frameCounter) { respondError(cmd, "frame " + std::to_string(c.frame) + " already passed (current " + std::to_string(frameCounter) + ")"); return true; }
        intRunToActive = true;
        intRunToFrame = c.frame;
        std::string why = runUntilHalt();
        char b[160];
        snprintf(b, sizeof b, "{\"ok\":true,\"cmd\":\"run_to\",\"frame\":%u,\"target\":%u,\"halted\":true,\"halt_reason\":\"%s\"}",
          frameCounter, c.frame, jsonEscape(why).c_str());
        respondRaw(b);
      }
      else if(cmd == "step") {
        if(!intHalted) { respondError(cmd, "busy: emulation running"); return true; }
        uint32 n = c.hasCount ? c.count : 1;
        if(n == 0) { respondError(cmd, "step requires n >= 1"); return true; }
        intStepPending = n;
        std::string why = runUntilHalt();
        char b[192];
        snprintf(b, sizeof b, "{\"ok\":true,\"cmd\":\"step\",\"n\":%u,\"frame\":%u,\"halted\":true,\"halt_reason\":\"%s\",\"pc\":%u}",
          n, frameCounter, jsonEscape(why).c_str(), (unsigned)SuperFamicom::cpu.r.pc.d);
        respondRaw(b);
      }
      else if(cmd == "bp_add" || cmd == "bp_del") {
        if(!c.hasKind || !c.hasAddr) { respondError(cmd, std::string(cmd) + " requires kind (exec|read|write) and addr"); return true; }
        char k = kindChar(c.kind);
        if(!k) { respondError(cmd, "kind must be exec|read|write"); return true; }
        uint8 id; uint32 off;
        if(!decodePhysical(c.addr, id, off)) {
          respondError(cmd, "addr " + std::to_string(c.addr) + " maps to open bus (unmapped) — cannot arm");
          return true;
        }
        if(cmd == "bp_add") {
          for(auto& bp : intBreakpoints) {
            if(bp.kind == k && bp.id == id && bp.offset == off) {
              char b[128];
              snprintf(b, sizeof b, "{\"ok\":true,\"cmd\":\"bp_add\",\"duplicate\":true,\"kind\":\"%s\",\"addr\":%u,\"mirrors\":\"all\"}", kindName(k), c.addr);
              respondRaw(b);
              return true;
            }
          }
          intBreakpoints.push_back({k, id, off, c.addr});
          char b[128];
          snprintf(b, sizeof b, "{\"ok\":true,\"cmd\":\"bp_add\",\"kind\":\"%s\",\"addr\":%u,\"mirrors\":\"all\"}", kindName(k), c.addr);
          respondRaw(b);
        } else {
          bool removed = false;
          for(uint i = 0; i < intBreakpoints.size(); i++) {
            auto& bp = intBreakpoints[i];
            if(bp.kind == k && bp.id == id && bp.offset == off) {
              intBreakpoints.erase(intBreakpoints.begin() + i);
              removed = true;
              break;  //bp_del removes ONE armed entry per call
            }
          }
          char b[160];
          snprintf(b, sizeof b, "{\"ok\":true,\"cmd\":\"bp_del\",\"removed\":%s,\"kind\":\"%s\",\"addr\":%u}", removed ? "true" : "false", kindName(k), c.addr);
          respondRaw(b);
        }
      }
      else if(cmd == "mem_read") {
        if(!c.hasAddr || !c.hasLen) { respondError(cmd, "mem_read requires addr and len"); return true; }
        if(c.len > 4096) { respondError(cmd, "len capped at 4096 per mem_read"); return true; }
        if(c.len == 0) { respondError(cmd, "len must be >= 1"); return true; }
        std::string out = "{\"ok\":true,\"cmd\":\"mem_read\",\"addr\":" + std::to_string(c.addr) + ",\"len\":" + std::to_string(c.len) + ",\"data\":";
        //block-buffered so the response string is built once (no realloc churn)
        std::vector<uint8> buf(c.len);
        for(uint32 i = 0; i < c.len; i++) buf[i] = SuperFamicom::cpu.readDisassembler(c.addr + i);
        out += b64Field(buf.data(), buf.size());
        out += "}";
        respondRaw(out);
      }
      else if(cmd == "wram") {
        std::string out = "{\"ok\":true,\"cmd\":\"wram\",\"addr\":7196160,\"len\":131072,\"data\":";
        out += b64Field(SuperFamicom::cpu.wram, sizeof(SuperFamicom::cpu.wram));
        out += "}";
        respondRaw(out);
      }
      else if(cmd == "vram") {
        std::string out = "{\"ok\":true,\"cmd\":\"vram\",\"words\":65536,\"data\":";
        static std::vector<uint8> buf;
        buf.resize(128 * 1024);
        for(uint i = 0; i < 64 * 1024; i++) {
          uint16 w = SuperFamicom::ppu.vramWord(i);
          buf[2 * i] = w & 0xFF; buf[2 * i + 1] = w >> 8;
        }
        out += b64Field(buf.data(), buf.size());
        out += "}";
        respondRaw(out);
      }
      else if(cmd == "cgram") {
        if(!accuratePPU) {
          respondError(cmd, "cgram requires --accurate (fast renderer keeps its own palette copy)");
          return true;
        }
        std::string out = "{\"ok\":true,\"cmd\":\"cgram\",\"colors\":256,\"data\":";
        uint8 buf[512];
        for(uint i = 0; i < 256; i++) {
          uint16 col = SuperFamicom::ppu.cgramColor(i);
          buf[2 * i] = col & 0xFF; buf[2 * i + 1] = col >> 8;
        }
        out += b64Field(buf, sizeof buf);
        out += "}";
        respondRaw(out);
      }
      else if(cmd == "frame") {
        if(lastFrame.empty()) { respondError(cmd, "no frame captured yet"); return true; }
        if(c.hasPath) {
          //RFMR file (same layout as --frame): magic, w, h, pitch_bytes, words
          FILE* f = fopen(c.path.c_str(), "wb");
          if(!f) { respondError(cmd, "cannot open " + c.path); return true; }
          uint32 hdr[4] = {0x524D4652, lastFrameW, lastFrameH, lastFramePitch};
          fwrite(hdr, 4, 4, f);
          for(uint y = 0; y < lastFrameH; y++)
            fwrite(lastFrame.data() + (size_t)y * (lastFramePitch / 2), 2, lastFrameW, f);
          fclose(f);
          char b[192];
          snprintf(b, sizeof b, "{\"ok\":true,\"cmd\":\"frame\",\"path\":\"%s\",\"w\":%u,\"h\":%u,\"pitch\":%u,\"format\":\"rgb555-rfmr\"}",
            jsonEscape(c.path).c_str(), lastFrameW, lastFrameH, lastFramePitch);
          respondRaw(b);
        } else {
          //inline base64 (w*h words, row stride = pitch/2 words)
          std::vector<uint8> flat((size_t)lastFrameW * lastFrameH * 2);
          for(uint y = 0; y < lastFrameH; y++)
            memcpy(flat.data() + (size_t)y * lastFrameW * 2, lastFrame.data() + (size_t)y * (lastFramePitch / 2), (size_t)lastFrameW * 2);
          std::string out = "{\"ok\":true,\"cmd\":\"frame\",\"w\":" + std::to_string(lastFrameW) + ",\"h\":" + std::to_string(lastFrameH) + ",\"format\":\"rgb555\",\"data\":";
          out += b64Field(flat.data(), flat.size());
          out += "}";
          respondRaw(out);
        }
      }
      else if(cmd == "state_save") {
        std::string path = c.hasPath ? c.path : "/tmp/bsnes-oracle/session.state";
        //deterministic serialization (synchronize=false): the snapshot is taken
        //at the exact halt point — no sync-point advance. Requires a
        //co_serializable libco (amd64: yes).
        serializer s = emulator->serialize(/* synchronize = */ false);
        if(!s.size() || !file::write(path.c_str(), array_view<uint8_t>(s.data(), s.size()))) {
          respondError(cmd, "state save FAILED: " + path);
          return true;
        }
        char b[256];
        snprintf(b, sizeof b, "{\"ok\":true,\"cmd\":\"state_save\",\"path\":\"%s\",\"bytes\":%u,\"frame\":%u}",
          jsonEscape(path).c_str(), (unsigned)s.size(), frameCounter);
        respondRaw(b);
      }
      else if(cmd == "state_load") {
        if(!c.hasPath) { respondError(cmd, "state_load requires path"); return true; }
        auto data = file::read(c.path.c_str());
        if(!data.size()) { respondError(cmd, "cannot read " + c.path); return true; }
        serializer s(data.data(), data.size());
        emulator->unload();
        if(!emulator->load()) { respondError(cmd, "reload FAILED after unload"); return true; }
        //power() re-runs System::power() which re-inits serializeSize[2] for
        //BOTH modes (batch --state-load gets away without it only because the
        //batch flow powers the machine in main() BEFORE its load).
        emulator->power();
        if(!emulator->unserialize(s)) {
          respondError(cmd, "unserialize FAILED (renderer-mode mismatch or stale state; regenerate against this binary)");
          //back on solid ground for further introspection
          return true;
        }
        char b[256];
        snprintf(b, sizeof b, "{\"ok\":true,\"cmd\":\"state_load\",\"path\":\"%s\",\"bytes\":%u,\"frame\":%u}",
          jsonEscape(c.path).c_str(), (unsigned)data.size(), frameCounter);
        respondRaw(b);
      }
      else {
        respondError(cmd, "unknown command (vocabulary: run_to step bp_add bp_del regs mem_read wram vram cgram frame state_save state_load status quit)");
      }
      return true;
    };

    fprintf(stderr, "[oracle] interactive JSON channel ready (stdin->stdout)\n");
    respondRaw("{\"ok\":true,\"cmd\":\"hello\",\"oracle\":\"bsnes-oracle\",\"frame\":0,\"vocabulary\":[\"run_to\",\"step\",\"bp_add\",\"bp_del\",\"regs\",\"mem_read\",\"wram\",\"vram\",\"cgram\",\"frame\",\"state_save\",\"state_load\",\"status\",\"quit\"]}");

    std::string pending;
    bool eof = false;
    while(!intQuitRequested && !eof) {
      //Read LINE-at-a-time, not fread-to-buffer: fread blocks until the full
      //64K chunk or EOF, which turned the channel into script-mode (a
      //lockstep driver deadlocks on its first command — caught by the
      //dispatcher's conversational smoke 2026-10-13). fgets returns as soon
      //as one newline arrives: true interactive/attachable use.
      static char linebuf[65536 + 1];
      while(fgets(linebuf, (int)sizeof linebuf, stdin)) {
        pending += linebuf;
        size_t pos;
        while((pos = pending.find('\n')) != std::string::npos) {
          std::string line = pending.substr(0, pos);
          pending.erase(0, pos + 1);
          if(!line.empty()) {
            if(!execCommand(line)) { intQuitRequested = true; break; }
          }
        }
        if(intQuitRequested) break;
      }
      eof = true;
    }
    if(eof && !intQuitRequested) {
      //EOF = clean shutdown, same as quit
      respondRaw("{\"ok\":true,\"cmd\":\"eof\",\"bye\":true}");
    }
    fprintf(stderr, "[oracle] interactive session end: frames=%u halt_reason=%s\n", frameCounter, intHaltReason.c_str());
    //interactive exit: epilogue below runs at the final halt point (WRAM/
    //state-to-file/peeks) with exitCode 0. quit=true SKIPS the batch run
    //loop — falling through would ghost-run the emulator to the frame
    //budget after the channel closed (found on the final smoke: rc 3).
    quit = true;
  }

  // ---------------------------------------------------------------- run loop
  while(!quit) {
    emulator->run();

    if(SuperFamicom::scheduler.event == SuperFamicom::Scheduler::Event::Debug) {
      if(lastDebugWasSpcStep) {
        //SPC-side step or breakpoint event
        bool isBp = false;
        for(auto bp : spcExecBreakpoints) if(bp == SuperFamicom::smp.r.pc.w) isBp = true;
        if(isBp) {
          breakHits++;
          fprintf(stderr, "\n[spc-break] hit #%" PRIu64 " at frame %u\n", breakHits, frameCounter);
          reportSMP("       ");
          reportSpcContext();
          if(stepsRemaining > 0) {
            stepArm = true;
          } else if(continueAfterHit) {
            if(breakHits >= maxBreakHits) { exitCode = 4; quit = true; }
          } else {
            quit = true;
          }
        } else {
          stepHits++;
          reportSMP("[spc-step]");
          reportSpcContext();
          if(stepsRemaining > 0) stepArm = true;
          if(stepsRemaining > 0) stepsRemaining--;
          else quit = true;
        }
      } else if(lastDebugWasStep) {
        stepHits++;
        reportCPU("[step]");
        fprintf(stderr, "        %s\n", SuperFamicom::cpu.disassemble(
          SuperFamicom::cpu.r.pc.d, SuperFamicom::cpu.r.e,
          SuperFamicom::cpu.r.p.m, SuperFamicom::cpu.r.p.x).data());
        if(stepsRemaining > 0) stepArm = true;  //keep stepping
        //step sequences end by themselves when stepsRemaining hits 0
        if(stepsRemaining > 0) stepsRemaining--;
        else quit = true;
      } else {
        breakHits++;
        fprintf(stderr, "\n[break] hit #%" PRIu64 " at frame %u\n", breakHits, frameCounter);
        reportCPU("       ");
        reportContext();
        if(wramAtHit && !wramDumpPath.empty() && !wramDumped) dumpWRAM("break hit");
        if(stepsRemaining > 0) {
          stepArm = true;
        } else if(continueAfterHit) {
          if(breakHits >= maxBreakHits) { exitCode = 4; quit = true; }
        } else {
          quit = true;  //default: stop at the first real break
        }
      }
    } else {
      //frame event returned
      //VRAM journal window arming (frame-indexed)
      if(!vramJournalPath.empty()) {
        if(journalFromFrame && frameCounter == journalFromFrame && !SuperFamicom::ppu.journalArmed) {
          SuperFamicom::ppu.beginVramJournal();
          fprintf(stderr, "[oracle] VRAM journal armed at frame %u\n", frameCounter);
        }
        if(journalToFrame && SuperFamicom::ppu.journalArmed && frameCounter >= journalToFrame) {
          SuperFamicom::ppu.endVramJournal();
          SuperFamicom::ppu.journalArmed = 0;
          fprintf(stderr, "[oracle] VRAM journal closed at frame %u\n", frameCounter);
        }
      }
      std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - tStart;
      if(frameCounter >= frameBudget) { exitCode = 3; quit = true; }
      else if((uint)elapsed.count() >= wallClockBudget) { exitCode = 2; quit = true; }
    }
  }

  // ---------------------------------------------------------------- epilogue
  fprintf(stderr, "\n[oracle] SUMMARY: frames=%u break-hits=%" PRIu64 " step-hits=%" PRIu64 " exit=%d\n",
    frameCounter, breakHits, stepHits, exitCode);

  if(!wramDumpPath.empty() && !wramDumped) dumpWRAM("exit");
  if(!cgramDumpPath.empty()) dumpCGRAM("exit");
  if(!spcramDumpPath.empty()) dumpSPCRAM("exit");
  if(!dspRegsDumpPath.empty()) dumpDSPRegs("exit");
  if(!frameDumpPath.empty()) dumpFrame("exit");
  if(!vramDumpPath.empty()) dumpVRAM("exit");
  if(!ppuDumpPath.empty()) dumpPPU("exit");
  if(!vramJournalPath.empty()) dumpVramJournal("exit");
  if(!ppu2DumpPath.empty()) dumpPPU2("exit");
  for(auto& p : peeks) {
    fprintf(stderr, "[peek] $%06X:", p.first);
    for(uint i = 0; i < p.second; i++) fprintf(stderr, "%s%02X", i ? " " : "", SuperFamicom::cpu.readDisassembler(p.first + i));
    fprintf(stderr, "\n");
  }

  if(!stateSavePath.empty()) {
    serializer s = emulator->serialize(true);
    if(file::write(stateSavePath.c_str(), array_view<uint8_t>(s.data(), s.size()))) {
      fprintf(stderr, "[oracle] state saved to %s (%u bytes)\n", stateSavePath.c_str(), s.size());
    } else {
      fprintf(stderr, "[oracle] state save FAILED: %s\n", stateSavePath.c_str());
    }
  }
  //flush save RAM to disk next to the ROM (opt-in: --srm)
  emulator->save();
  if(program.flushSaveRAM && SuperFamicom::cartridge.ram.size()) {
    string srm = {program.baseName, ".srm"};
    if(file::write(srm, array_view<uint8_t>(SuperFamicom::cartridge.ram.data(), SuperFamicom::cartridge.ram.size()))) {
      if(!quiet) fprintf(stderr, "[oracle] save RAM flushed to %s (%u bytes)\n", srm.data(), SuperFamicom::cartridge.ram.size());
    }
  }

  emulator->unload();
  fprintf(stderr, "[oracle] done\n");
  return exitCode;
}
