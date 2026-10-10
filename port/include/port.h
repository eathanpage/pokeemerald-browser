#ifndef GUARD_PORT_H
#define GUARD_PORT_H

// Interface between the game code and the browser port's hardware layer.

#include <stdint.h>

void Port_Halt(const char *reason);
void Port_Log(const char *fmt, ...);
void Port_RunVideoFrame(void);

#endif // GUARD_PORT_H
