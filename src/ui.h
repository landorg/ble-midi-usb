#pragma once

#include "BleMidiParser.h"

void uiBegin();
void uiLoop();

// Non-blocking, called for each forwarded message.
void uiOnMidi(const UsbMidiPacket& pkt);
