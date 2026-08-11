#include <Syringe.h>
#include <Helpers/Macro.h>

#include "Runtime.h"

// IDA (gamemd.exe MD5 56d582a1d6f3c144d3adc867d7a4d91b):
// 0x55DE3A  89 0D 64 B5 A8 00  mov [0xA8B564], ecx
// This lies on the normal return path of MainLoop (0x55D360), after the frame
// counters are calculated and before engine housekeeping calls. It executes
// once per normal frame. Current public Phobos uses other MainLoop addresses
// (0x55D360/0x55D871/0x55DEC1/0x55DED5), not this instruction.
DEFINE_HOOK(0x55DE3A, RA2Hook_RuntimeTick, 0x6)
{
    Runtime::Tick();
    return 0;
}
