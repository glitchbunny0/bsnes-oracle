#include "SPC_DSP.h"

struct DSP {
  shared_pointer<Emulator::Stream> stream;
  uint8 apuram[64 * 1024] = {};

  //oracle: read-only APU RAM access for the --spcram dump.
  //The cgramColor() pattern: a small public const accessor next to the member;
  //the headless driver never reaches into private structs.
  //The full 64K apuram is the APU's whole world: SMP program + data + $FFC0 IPL
  //mirror + DSP registers + port files. (The echoram shadow is a DSP hack copy
  //and is NOT dumped.)
  auto apuramByte(uint16 address) const -> uint8 { return apuram[address]; }

  //oracle: read-only DSP register file ($5D BRR source directory
  //page lives HERE — internal DSP state, not in apuram).
  auto dspReg(uint8 address) const -> uint8 { return spc_dsp.reg(address); }

  auto main() -> void;
  auto read(uint8 address) -> uint8;
  auto write(uint8 address, uint8 data) -> void;

  auto load() -> bool;
  auto power(bool reset) -> void;
  auto mute() -> bool;

  auto serialize(serializer&) -> void;

  int64 clock = 0;

private:
  bool fastDSP = false;
  SPC_DSP spc_dsp;
  int16 samplebuffer[8192];

//unserialized:
  uint8 echoram[64 * 1024] = {};
};

extern DSP dsp;
