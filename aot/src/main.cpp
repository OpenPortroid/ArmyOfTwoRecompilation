// armyoftworecomp - ReXGlue Recompiled Project

#include "generated/default/armyoftworecomp_init.h"

#include "armyoftworecomp_app.h"

REX_DEFINE_APP(armyoftworecomp, ArmyoftworecompApp::Create)

#ifdef _WIN32
// Ask hybrid-graphics drivers (NVIDIA Optimus / AMD PowerXpress) to run the
// game on the discrete GPU. Drivers only honor these exports from the .exe.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif
