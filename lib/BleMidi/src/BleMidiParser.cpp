#include "BleMidiParser.h"

BleMidiParser::BleMidiParser(Sink sink, void* ctx, uint8_t cable)
    : sink_(sink), ctx_(ctx), cable_(cable & 0x0F) {}

void BleMidiParser::reset() {
    status_ = 0;
    count_ = 0;
    inSysex_ = false;
    sysexCount_ = 0;
}

void BleMidiParser::parse(const uint8_t* data, size_t len) {
    // Header byte: bit7 = 1, bit6 = 0, bits 5..0 = timestamp high.
    if (len < 2 || (data[0] & 0xC0) != 0x80) return;

    // After the header, a byte with bit7 set is a timestamp unless it directly
    // follows a timestamp, in which case it is a status byte. Data bytes
    // (bit7 clear) are running-status data or SysEx continuation.
    bool afterTimestamp = false;
    for (size_t i = 1; i < len; i++) {
        uint8_t b = data[i];
        if ((b & 0x80) && !afterTimestamp) {
            afterTimestamp = true;
            continue;
        }
        afterTimestamp = false;
        feed(b);
    }
}

void BleMidiParser::emit(uint8_t cin, uint8_t b0, uint8_t b1, uint8_t b2) {
    UsbMidiPacket pkt = {{static_cast<uint8_t>((cable_ << 4) | cin), b0, b1, b2}};
    sink_(pkt, ctx_);
}

void BleMidiParser::feed(uint8_t b) {
    // Real-time: single byte, may appear anywhere (even inside SysEx).
    if (b >= 0xF8) {
        emit(0xF, b);
        return;
    }

    if (b == 0xF0) {
        inSysex_ = true;
        sysex_[0] = b;
        sysexCount_ = 1;
        status_ = 0;
        return;
    }

    if (b == 0xF7) {
        if (!inSysex_) return;
        sysex_[sysexCount_++] = b;
        // CIN 0x5/0x6/0x7: SysEx ends with 1/2/3 bytes
        emit(0x4 + sysexCount_, sysex_[0], sysexCount_ > 1 ? sysex_[1] : 0,
             sysexCount_ > 2 ? sysex_[2] : 0);
        inSysex_ = false;
        sysexCount_ = 0;
        return;
    }

    if (inSysex_) {
        if (b & 0x80) {
            // Any other status byte aborts the SysEx.
            inSysex_ = false;
            sysexCount_ = 0;
        } else {
            sysex_[sysexCount_++] = b;
            if (sysexCount_ == 3) {
                emit(0x4, sysex_[0], sysex_[1], sysex_[2]);  // SysEx continues
                sysexCount_ = 0;
            }
            return;
        }
    }

    if (b & 0x80) {
        count_ = 0;
        if (b < 0xF0) {
            uint8_t type = b & 0xF0;
            status_ = b;
            needed_ = (type == 0xC0 || type == 0xD0) ? 1 : 2;
            return;
        }
        status_ = 0;  // system common cancels running status
        switch (b) {
            case 0xF1:
            case 0xF3:
                status_ = b;
                needed_ = 1;
                break;
            case 0xF2:
                status_ = b;
                needed_ = 2;
                break;
            case 0xF6:
                emit(0x5, b);
                break;
            default:  // 0xF4, 0xF5 undefined
                break;
        }
        return;
    }

    if (status_ == 0) return;  // data byte without status
    data_[count_++] = b;
    if (count_ < needed_) return;

    uint8_t cin;
    if (status_ < 0xF0) {
        cin = status_ >> 4;
    } else {
        cin = needed_ == 1 ? 0x2 : 0x3;  // system common, 2 or 3 bytes
    }
    emit(cin, status_, data_[0], needed_ > 1 ? data_[1] : 0);
    count_ = 0;
    if (status_ >= 0xF0) status_ = 0;  // only channel messages run on
}
