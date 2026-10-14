struct PPU : Thread, PPUcounter {
  alwaysinline auto interlace() const -> bool { return display.interlace; }
  alwaysinline auto overscan() const -> bool { return display.overscan; }
  alwaysinline auto vdisp() const -> uint { return display.vdisp; }

  //ppu.cpp
  PPU();
  ~PPU();

  auto synchronizeCPU() -> void;
  static auto Enter() -> void;
  auto load() -> bool;
  auto power(bool reset) -> void;

  //main.cpp
  auto main() -> void;
  noinline auto cycleObjectEvaluate() -> void;
  template<uint Cycle> noinline auto cycleBackgroundFetch() -> void;
  noinline auto cycleBackgroundBegin() -> void;
  noinline auto cycleBackgroundBelow() -> void;
  noinline auto cycleBackgroundAbove() -> void;
  noinline auto cycleRenderPixel() -> void;
  template<uint> auto cycle() -> void;

  //io.cpp
  auto latchCounters(uint hcounter, uint vcounter) -> void;
  auto latchCounters() -> void;

  //soe-re oracle: palette RAM access for --cgram dump (read-only)
  auto cgramColor(uint index) const -> uint15 { return screen.cgram[index]; }

  //oracle: VRAM + BG screen-base visibility for tilemap work
  auto vramWord(uint address) const -> uint16 { return vram[address]; }
  auto bgMode() const -> uint8 { return io.bgMode; }
  auto bgPriorityFlag() const -> uint8 { return io.bgPriority; }
  struct BgState { uint16 screenAddress; uint16 tiledataAddress; uint2 screenSize; uint1 tileSize; uint16 hoffset; uint16 voffset; uint1 aboveEnable; uint1 belowEnable; };
  auto bgState(uint id) const -> BgState {
    auto& bg = id == 0 ? bg1 : id == 1 ? bg2 : id == 2 ? bg3 : bg4;
    return {bg.io.screenAddress, bg.io.tiledataAddress, bg.io.screenSize, bg.io.tileSize, bg.io.hoffset, bg.io.voffset, bg.io.aboveEnable, bg.io.belowEnable};
  }
  auto beginVramJournal() -> void { journalArmed = true; }
  //fix: closing must NOT journal.reset() — the driver dumps the buffer at
  //run end; resetting here wiped all recorded entries before the dump (the
  //0-entry anomaly). Disarm only; reset belongs to whoever consumed the data.
  auto endVramJournal() -> void { journalArmed = false; }

  //oracle: read-only register visibility for the extended
  //PPU dump (--ppu2). Same pattern as cgramColor(): small const accessors,
  //no private-struct reaching from the driver.
  auto inidispDisable() const -> uint1 { return io.displayDisable; }
  auto inidispBrightness() const -> uint4 { return io.displayBrightness; }
  auto oamByte(uint10 a) const -> uint8;
  //defined in ppu.cpp: OAM::read is non-const (latch side-effects); replicate its field decode here
  auto objReg(uint i) const -> uint8 {
    switch(i) {
    case 0: return obj.io.baseSize;
    case 1: return obj.io.nameselect;
    case 2: return obj.io.tiledataAddress >> 0;
    case 3: return obj.io.tiledataAddress >> 8;
    case 4: return obj.io.firstSprite;
    case 5: return obj.io.priority[0];
    case 6: return obj.io.priority[1];
    case 7: return obj.io.priority[2];
    case 8: return obj.io.priority[3];
    case 9: return obj.io.aboveEnable;
    case 10: return obj.io.belowEnable;
    }
    return 0;
  }
  auto windowReg(uint i) const -> uint8 {
    switch(i) {
    case 0:  return window.io.bg1.oneEnable;  case 1:  return window.io.bg1.oneInvert;
    case 2:  return window.io.bg1.twoEnable;  case 3:  return window.io.bg1.twoInvert;
    case 4:  return window.io.bg2.oneEnable;  case 5:  return window.io.bg2.oneInvert;
    case 6:  return window.io.bg2.twoEnable;  case 7:  return window.io.bg2.twoInvert;
    case 8:  return window.io.bg3.oneEnable;  case 9:  return window.io.bg3.oneInvert;
    case 10: return window.io.bg3.twoEnable;  case 11: return window.io.bg3.twoInvert;
    case 12: return window.io.bg4.oneEnable;  case 13: return window.io.bg4.oneInvert;
    case 14: return window.io.bg4.twoEnable;  case 15: return window.io.bg4.twoInvert;
    case 16: return window.io.obj.oneEnable;  case 17: return window.io.obj.oneInvert;
    case 18: return window.io.obj.twoEnable;  case 19: return window.io.obj.twoInvert;
    case 20: return window.io.col.oneEnable;  case 21: return window.io.col.oneInvert;
    case 22: return window.io.col.twoEnable;  case 23: return window.io.col.twoInvert;
    case 24: return window.io.oneLeft;        case 25: return window.io.oneRight;
    case 26: return window.io.twoLeft;        case 27: return window.io.twoRight;
    case 28: return window.io.bg1.mask;       case 29: return window.io.bg2.mask;
    case 30: return window.io.bg3.mask;       case 31: return window.io.bg4.mask;
    case 32: return window.io.obj.mask;       case 33: return window.io.col.mask;
    case 34: return window.io.col.aboveMask;  case 35: return window.io.col.belowMask;
    }
    return 0;
  }
  auto screenReg(uint i) const -> uint8 {
    switch(i) {
    case 0: return screen.io.directColor;
    case 1: return screen.io.blendMode;
    case 2: return screen.io.colorHalve;
    case 3: return screen.io.colorMode;
    case 4: return screen.io.colorRed;
    case 5: return screen.io.colorGreen;
    case 6: return screen.io.colorBlue;
    case 7:  return screen.io.bg1.colorEnable;
    case 8:  return screen.io.bg2.colorEnable;
    case 9:  return screen.io.bg3.colorEnable;
    case 10: return screen.io.bg4.colorEnable;
    case 11: return screen.io.obj.colorEnable;
    case 12: return screen.io.back.colorEnable;
    }
    return 0;
  }

  //VRAM-write journal: records {vram address, byte index(0=low,1=high), cpuOrigin} per byte write
  struct VramJournalEntry { uint16 address; uint1 byte; uint1 cpu; };
  vector<VramJournalEntry> journal;
  uint1 journalArmed = 0;
  uint1 journalCpu = 0;  //set around CPU::write's bus.write; DMA writes leave it 0

  //serialization.cpp
  auto serialize(serializer&) -> void;

private:
  //ppu.cpp
  alwaysinline auto step() -> void;
  alwaysinline auto step(uint clocks) -> void;

  //io.cpp
  alwaysinline auto addressVRAM() const -> uint16;
  alwaysinline auto readVRAM() -> uint16;
  alwaysinline auto writeVRAM(bool byte, uint8 data) -> void;
  alwaysinline auto readOAM(uint10 address) -> uint8;
  alwaysinline auto writeOAM(uint10 address, uint8 data) -> void;
  alwaysinline auto readCGRAM(bool byte, uint8 address) -> uint8;
  alwaysinline auto writeCGRAM(uint8 address, uint15 data) -> void;
  auto readIO(uint address, uint8 data) -> uint8;
  auto writeIO(uint address, uint8 data) -> void;
  auto updateVideoMode() -> void;

  struct VRAM {
    uint16 data[64 * 1024];
    uint16 mask = 0x7fff;
    auto operator[](uint address) -> uint16& { return data[address & mask]; }
    auto operator[](uint address) const -> const uint16& { return data[address & mask]; }
  } vram;

  uint16 output[512 * 480];
  uint16 lightTable[16][32768];

  struct {
    bool interlace;
    bool overscan;
    uint vdisp;
  } display;

  auto refresh() -> void;

  struct {
    uint4 version;
    uint8 mdr;
  } ppu1, ppu2;

  struct Latch {
    uint16 vram;
     uint8 oam;
     uint8 cgram;
     uint8 bgofsPPU1;
     uint3 bgofsPPU2;
     uint8 mode7;
     uint1 counters;
     uint1 hcounter;
     uint1 vcounter;

    uint10 oamAddress;
     uint8 cgramAddress;
  } latch;

  struct IO {
    //$2100  INIDISP
     uint1 displayDisable;
     uint4 displayBrightness;

    //$2102  OAMADDL
    //$2103  OAMADDH
    uint10 oamBaseAddress;
    uint10 oamAddress;
     uint1 oamPriority;

    //$2105  BGMODE
     uint1 bgPriority;
     uint8 bgMode;

    //$210d  BG1HOFS
    uint16 hoffsetMode7;

    //$210e  BG1VOFS
    uint16 voffsetMode7;

    //$2115  VMAIN
     uint1 vramIncrementMode;
     uint2 vramMapping;
     uint8 vramIncrementSize;

    //$2116  VMADDL
    //$2117  VMADDH
    uint16 vramAddress;

    //$211a  M7SEL
     uint2 repeatMode7;
     uint1 vflipMode7;
     uint1 hflipMode7;

    //$211b  M7A
    uint16 m7a;

    //$211c  M7B
    uint16 m7b;

    //$211d  M7C
    uint16 m7c;

    //$211e  M7D
    uint16 m7d;

    //$211f  M7X
    uint16 m7x;

    //$2120  M7Y
    uint16 m7y;

    //$2121  CGADD
     uint8 cgramAddress;
     uint1 cgramAddressLatch;

    //$2133  SETINI
     uint1 extbg;
     uint1 pseudoHires;
     uint1 overscan;
     uint1 interlace;

    //$213c  OPHCT
    uint16 hcounter;

    //$213d  OPVCT
    uint16 vcounter;
  } io;

  #include "mosaic.hpp"
  #include "background.hpp"
  #include "object.hpp"
  #include "window.hpp"
  #include "screen.hpp"

  Mosaic mosaic;
  Background bg1;
  Background bg2;
  Background bg3;
  Background bg4;
  Object obj;
  Window window;
  Screen screen;

  friend class PPU::Background;
  friend class PPU::Object;
  friend class PPU::Window;
  friend class PPU::Screen;
  friend class System;
  friend class PPUfast;
};

extern PPU ppu;
