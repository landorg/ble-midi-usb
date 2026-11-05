#pragma once

#include <cstddef>
#include <cstdint>

// USB-MIDI 1.0 event packet: [cable << 4 | CIN, midi0, midi1, midi2]
struct UsbMidiPacket {
    uint8_t b[4];
};

// Decodes BLE MIDI characteristic notifications (header + timestamp bytes,
// running status, SysEx split across packets) into USB-MIDI event packets.
// Timestamps are dropped: messages are forwarded as soon as they arrive.
class BleMidiParser {
public:
    using Sink = void (*)(const UsbMidiPacket& pkt, void* ctx);

    BleMidiParser(Sink sink, void* ctx, uint8_t cable = 0);

    // One BLE notification payload.
    void parse(const uint8_t* data, size_t len);

    // Forget running status and any partial message (call on disconnect).
    void reset();

private:
    void feed(uint8_t b);
    void emit(uint8_t cin, uint8_t b0, uint8_t b1 = 0, uint8_t b2 = 0);

    Sink sink_;
    void* ctx_;
    uint8_t cable_;

    uint8_t status_ = 0;  // status of the message being assembled, 0 = none
    uint8_t data_[2] = {};
    uint8_t count_ = 0;
    uint8_t needed_ = 0;

    bool inSysex_ = false;
    uint8_t sysex_[3] = {};
    uint8_t sysexCount_ = 0;
};
