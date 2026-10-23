// armyoftworecomp - ReXGlue Recompiled Project

#include "generated/default/armyoftworecomp_init.h"

#include "armyoftworecomp_app.h"

REX_DEFINE_APP(armyoftworecomp, ArmyoftworecompApp::Create)

#if defined(_WIN32) && defined(_M_X64)
#include <math.h>

// The x64 CRT picks FMA3 versions of its math functions on CPUs that have FMA3
// (not the ones the compat build is for), and they round some results
// differently (sin, pow, exp2 of double; the float log2f the recompiled code
// calls gave the same bits both ways on a Ryzen 8400F). Co-op runs the same
// simulation on two PCs, so every PC takes the plain versions: set before
// main, ahead of every thread.
static const int g_no_fma3_math = _set_FMA3_enable(0);
#endif

#ifdef _WIN32
// Ask hybrid-graphics drivers (NVIDIA Optimus / AMD PowerXpress) to run the
// game on the discrete GPU. Drivers only honor these exports from the .exe.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif
