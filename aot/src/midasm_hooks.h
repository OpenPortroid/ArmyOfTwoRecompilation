// midasm_hooks.h - Declarations for [[midasm_hook]] entries in midasm_hooks.toml.
// The generated code emits its own extern declarations for these.
#pragma once

#include <rex/ppc.h>

void AO2FrameLimiterHook(PPCRegister &f0, PPCRegister &f1, PPCRegister &f30);
void AO2PresentIntervalHook(PPCRegister &r10);
void AO2AnisotropyHook(PPCRegister &r5);
