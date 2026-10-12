# Local PSL1GHT librsx command writer

This directory vendors the PSL1GHT `ppu/librsx/commands.c`,
`commands_impl.h`, `rsx_internal.h`, and `ppu/include/rsx/rsx_function_macros.h`
from source revision `e13aa539c07abe86cdbcd095dc1340492dbca213` (the installed
toolchain checkout used by the project). The upstream MIT license is retained
in `LICENSE`.

The local change repairs `rsxContextCallback` for the callback field's
`ATTRIBUTE_PRXPTR` OPD32 representation. The trampoline keeps `context` and
`count` in `r3` and `r4`, loads the 32-bit entry and TOC from the descriptor,
preserves stack and caller TOC, models volatile GPR/FPR/CR state as clobbered,
and returns the callback's `r3` value. The reservation macro retains upstream
zero-success/nonzero-failure behavior. The stage 2 build compiles this command
writer into the EBOOT ahead of `-lrsx`, so the static archive's old
`commands.o` is not selected.
