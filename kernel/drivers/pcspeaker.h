#ifndef DRIVERS_PCSPEAKER_H
#define DRIVERS_PCSPEAKER_H

#include <arch/types.h>

void pcspeaker_init(void);

//play a tone at the specified frequency (in Hz)
void pcspeaker_beep(uint32 freq);

//stop the speaker
void pcspeaker_stop(void);

#endif
