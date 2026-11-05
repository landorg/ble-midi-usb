#pragma once

#include <cstddef>

#include "BleMidiParser.h"

enum class MidiKind { NoteOn, NoteOff, Control, Bend, Pressure, Program, SysEx, System, Other };

// One line of text for a USB-MIDI packet, for showing on a screen. Channels
// count from 1, and middle C (note 60) is C4.
MidiKind formatUsbMidi(const UsbMidiPacket& pkt, char* out, size_t size);
