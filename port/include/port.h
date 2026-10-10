#ifndef GUARD_PORT_H
#define GUARD_PORT_H

// Interface between the game code and the browser port's hardware layer.

#include <stdint.h>

void Port_Halt(const char *reason);
void Port_Log(const char *fmt, ...);
void Port_RunVideoFrame(void);

// Called when the game polls for something that an interrupt would change.
// On the GBA, interrupts keep firing while the game spins in a loop; the port
// can't preempt, so after enough polls in one frame it runs a video frame
// (with its interrupts) from inside the loop.
void Port_BusyWait(void);

#endif // GUARD_PORT_H
