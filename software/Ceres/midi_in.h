#pragma once
#include <cstdint>

//
// MIDI byte parser for Ceres. Replaces libDaisy's, which drops a message when a realtime
// byte (clock!) lands in the middle of it, and has no notion of arrival time.
//
//   - Realtime bytes (0xF8-0xFF) are handled wherever they appear and leave the message
//     they interrupt intact.
//   - Running status, SysEx skipping, system common messages cancel running status.
//   - Every message carries the arrival time of its last byte (in audio samples).
//
// Plain C++ so it runs in the host tests.
//

namespace ceres
{

struct MidiMsg
{
    enum Type : uint8_t
    {
        CLOCK,
        START,
        CONTINUE,
        STOP,
        SONG_POSITION,  // value(): position in 16th notes since the start of the song
        CC,             // data1 = controller, data2 = value
        NOTE_ON,        // data1 = note, data2 = velocity (> 0)
        NOTE_OFF,       // also note on with velocity 0
        PROGRAM,        // data1 = program
    };

    Type     type;
    uint8_t  channel;  // 0-15, channel messages only
    uint8_t  data1, data2;
    double   time;     // Arrival time of the last byte, in samples

    uint16_t value() const { return (uint16_t)(data1 | (data2 << 7)); }
};


class MidiParser
{
  public:
    void Reset()
    {
        status_   = 0;
        needed_   = 0;
        received_ = 0;
        in_sysex_ = false;
    }

    // Feed one byte; returns true and fills `msg` when it completes a message
    bool Feed(uint8_t b, double time, MidiMsg& msg)
    {
        msg.time    = time;
        msg.channel = 0;
        msg.data1 = msg.data2 = 0;

        if (b >= 0xF8) {  // Realtime: may appear anywhere, even inside other messages
            switch (b) {
                case 0xF8: msg.type = MidiMsg::CLOCK; return true;
                case 0xFA: msg.type = MidiMsg::START; return true;
                case 0xFB: msg.type = MidiMsg::CONTINUE; return true;
                case 0xFC: msg.type = MidiMsg::STOP; return true;
                default: return false;  // Active sensing, reset, undefined
            }
        }

        if (b >= 0x80) {  // Status byte: starts a new message
            received_ = 0;
            in_sysex_ = b == 0xF0;
            if (b < 0xF0) {
                status_ = b;  // Channel message, stays as running status
                needed_ = (b & 0xE0) == 0xC0 ? 1 : 2;  // Program change / channel pressure: 1 byte
            } else {
                // System common: cancels running status
                status_ = b;
                needed_ = b == 0xF2 ? 2 : (b == 0xF1 || b == 0xF3 ? 1 : 0);
                if (needed_ == 0)
                    status_ = 0;  // F6 tune request, F7 end of SysEx: nothing to wait for
            }
            return false;
        }

        // Data byte
        if (in_sysex_ || status_ == 0)
            return false;
        data_[received_++] = b;
        if (received_ < needed_)
            return false;
        received_ = 0;

        msg.data1 = data_[0];
        msg.data2 = needed_ > 1 ? data_[1] : 0;

        if (status_ >= 0xF0) {
            uint8_t s = status_;
            status_   = 0;  // No running status for system common
            if (s == 0xF2) {
                msg.type = MidiMsg::SONG_POSITION;
                return true;
            }
            return false;  // MTC quarter frame, song select
        }

        msg.channel = status_ & 0x0F;
        switch (status_ & 0xF0) {
            case 0x80: msg.type = MidiMsg::NOTE_OFF; return true;
            case 0x90: msg.type = msg.data2 > 0 ? MidiMsg::NOTE_ON : MidiMsg::NOTE_OFF; return true;
            case 0xB0: msg.type = MidiMsg::CC; return true;
            case 0xC0: msg.type = MidiMsg::PROGRAM; return true;
            default: return false;  // Aftertouch, pitch bend: not used
        }
    }

  private:
    uint8_t status_ = 0, needed_ = 0, received_ = 0;
    uint8_t data_[2];
    bool    in_sysex_ = false;
};

} // namespace ceres
