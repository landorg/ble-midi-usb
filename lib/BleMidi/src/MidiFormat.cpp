#include "MidiFormat.h"

#include <cstdio>

static void noteName(uint8_t note, char* out, size_t size) {
    static const char* const kNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    snprintf(out, size, "%s%d", kNames[note % 12], note / 12 - 1);
}

static const char* realtimeName(uint8_t status) {
    switch (status) {
        case 0xF8: return "Clock";
        case 0xFA: return "Start";
        case 0xFB: return "Continue";
        case 0xFC: return "Stop";
        case 0xFE: return "Active Sensing";
        case 0xFF: return "Reset";
        default: return "Real-time";
    }
}

static MidiKind formatSystem(const UsbMidiPacket& pkt, char* out, size_t size) {
    const uint8_t cin = pkt.b[0] & 0x0F;
    const uint8_t a = pkt.b[1], b = pkt.b[2], c = pkt.b[3];

    if (cin == 0x0F) {
        snprintf(out, size, "%s", realtimeName(a));
        return MidiKind::System;
    }
    if (cin == 0x05 && a == 0xF6) {
        snprintf(out, size, "Tune Request");
        return MidiKind::System;
    }
    if (cin == 0x02 && a == 0xF1) {
        snprintf(out, size, "MTC Quarter Frame %d", b);
        return MidiKind::System;
    }
    if (cin == 0x02 && a == 0xF3) {
        snprintf(out, size, "Song Select %d", b);
        return MidiKind::System;
    }
    if (cin == 0x03 && a == 0xF2) {
        snprintf(out, size, "Song Position %d", b | (c << 7));
        return MidiKind::System;
    }
    switch (cin) {
        case 0x04:
        case 0x07:
            snprintf(out, size, "SysEx %02X %02X %02X", a, b, c);
            return MidiKind::SysEx;
        case 0x05:
            snprintf(out, size, "SysEx %02X", a);
            return MidiKind::SysEx;
        case 0x06:
            snprintf(out, size, "SysEx %02X %02X", a, b);
            return MidiKind::SysEx;
        default:
            snprintf(out, size, "%02X %02X %02X %02X", pkt.b[0], a, b, c);
            return MidiKind::Other;
    }
}

MidiKind formatUsbMidi(const UsbMidiPacket& pkt, char* out, size_t size) {
    const uint8_t status = pkt.b[1], d1 = pkt.b[2], d2 = pkt.b[3];
    if (status < 0x80 || status >= 0xF0) return formatSystem(pkt, out, size);

    const int ch = (status & 0x0F) + 1;
    char note[6];
    switch (status & 0xF0) {
        case 0x90:
            if (d2) {
                noteName(d1, note, sizeof(note));
                snprintf(out, size, "Ch %-2d Note On   %-4s vel %d", ch, note, d2);
                return MidiKind::NoteOn;
            }
            // Velocity 0 is a note off.
        case 0x80:
            noteName(d1, note, sizeof(note));
            snprintf(out, size, "Ch %-2d Note Off  %-4s", ch, note);
            return MidiKind::NoteOff;
        case 0xA0:
            noteName(d1, note, sizeof(note));
            snprintf(out, size, "Ch %-2d Poly AT   %-4s %d", ch, note, d2);
            return MidiKind::Pressure;
        case 0xB0:
            snprintf(out, size, "Ch %-2d CC %-3d    %d", ch, d1, d2);
            return MidiKind::Control;
        case 0xC0:
            snprintf(out, size, "Ch %-2d Program  %d", ch, d1);
            return MidiKind::Program;
        case 0xD0:
            snprintf(out, size, "Ch %-2d Pressure %d", ch, d1);
            return MidiKind::Pressure;
        default:
            snprintf(out, size, "Ch %-2d Bend     %+d", ch, (d1 | (d2 << 7)) - 8192);
            return MidiKind::Bend;
    }
}
