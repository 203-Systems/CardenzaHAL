#pragma once

#include <cstddef>
#include <cstdint>

// The USB device's pure decisions (usb/usb.h): which classes a firmware gets, the endpoints they take within
// the ESP32-S3's budget, the serial number, the serial port's lines and the esptool reset it recognises, and
// the playback's rate matching without a feedback endpoint. No ESP-IDF and no TinyUSB here: usb/usb.cpp uses
// it on the board, tests/usb_logic_test.cpp on the host.
namespace CardenzaHAL::USB::Logic {

// ---- Classes and endpoints -------------------------------------------------------------------------------
//
// The ESP32-S3's OTG controller has four IN endpoints besides EP0 (TinyUSB's DWC2 table: ep_in_count 5 with
// EP0), and an IN endpoint's TX FIFO is the one of its number, so IN endpoints are numbered 1 to 4. OUT
// endpoints share one RX FIFO and take any number from 1 to 6. What each class costs in IN endpoints:
inline constexpr int kInEndpoints = 4;
inline constexpr int kInMidi = 1;          // MIDI to the computer
inline constexpr int kInHid = 1;           // HID reports to the computer
inline constexpr int kInAudio = 1;         // the computer records
inline constexpr int kInAudioFeedback = 1; // the computer plays on the device's clock (else adaptive playback)
inline constexpr int kInSerial = 2;        // CDC-ACM: its notification endpoint and its data
inline constexpr int kInDrive = 1;         // mass storage (bulk, both ways)
inline constexpr int kInCamera = 1;        // the screen stream (bulk, on the WebUSB interface)
inline constexpr int kInWebUsb = 0;        // control requests on EP0 only
// Adaptive playback has no feedback endpoint: its OUT endpoint takes a number no IN endpoint can have.
inline constexpr uint8_t kAdaptiveAudioOut = 0x05;

struct Classes {
    bool midi = false;
    bool audio = false;
    bool playback = true;   // with audio: the computer plays to the device too (else it only records)
    bool feedback = false;  // with the playback
    bool hid = false;
    bool webusb = false;
    bool serial = false;
    bool drive = false;
    bool camera = false;
};

inline constexpr bool operator==(const Classes& a, const Classes& b)
{
    return a.midi == b.midi && a.audio == b.audio && a.playback == b.playback && a.feedback == b.feedback && a.hid == b.hid && a.webusb == b.webusb &&
           a.serial == b.serial && a.drive == b.drive && a.camera == b.camera;
}

inline constexpr int InEndpoints(const Classes& classes)
{
    return (classes.midi ? kInMidi : 0) + (classes.hid ? kInHid : 0) + (classes.audio ? kInAudio : 0) +
           (classes.audio && classes.playback && classes.feedback ? kInAudioFeedback : 0) + (classes.serial ? kInSerial : 0) +
           (classes.drive ? kInDrive : 0) + (classes.camera ? kInCamera : 0) + (classes.webusb ? kInWebUsb : 0);
}

// A class that carries the system channel (identify, info, launcher, download, the development channel): MIDI
// as SysEx, HID as report 0xCB, WebUSB as control requests, the serial port as lines. Audio, the drive and the
// camera carry none.
inline constexpr bool HasCommandChannel(const Classes& classes)
{
    return classes.midi || classes.hid || classes.webusb || classes.serial;
}

// The build's classes, as tusb_config.h derives them from Kconfig: the serial port is added when the build has
// no other command channel.
inline constexpr Classes Built(bool midi, bool audio, bool feedback, bool hid, bool webusb, bool serial, bool drive, bool camera)
{
    Classes classes;
    classes.midi = midi;
    classes.audio = audio;
    classes.playback = audio;
    classes.feedback = audio && feedback;
    classes.hid = hid;
    classes.webusb = webusb;
    classes.serial = serial || !(midi || hid || webusb);
    classes.drive = drive;
    classes.camera = camera;
    return classes;
}

// What the device presents: what the firmware asked for, within what the build has. A device always keeps a
// command channel, so a browser or a tool can recognise it and send it into flashing: without one, the serial
// port (the build has it then), else the command channels the build has.
inline constexpr Classes Effective(const Classes& asked, const Classes& built)
{
    Classes classes;
    classes.midi = asked.midi && built.midi;
    classes.audio = asked.audio && built.audio;
    classes.playback = classes.audio && asked.playback;
    classes.feedback = classes.playback && asked.feedback && built.feedback;
    classes.hid = asked.hid && built.hid;
    classes.webusb = asked.webusb && built.webusb;
    classes.serial = asked.serial && built.serial;
    classes.drive = asked.drive && built.drive;
    // The camera is the WebUSB interface's stream: it brings WebUSB with it.
    classes.camera = asked.camera && built.camera && built.webusb;
    if (classes.camera) classes.webusb = true;
    if (!HasCommandChannel(classes))
    {
        if (built.serial && !(built.midi || built.hid || built.webusb)) classes.serial = true;
        else if (built.webusb) classes.webusb = true;  // costs no endpoint
        else if (built.serial) classes.serial = true;
        else classes.midi = built.midi, classes.hid = !built.midi && built.hid;
    }
    return classes;
}

// What gives way when the classes need more IN endpoints than there are (a firmware lends the card or shows
// the camera): first the playback's feedback endpoint (the playback becomes adaptive: nothing is lost), then
// HID, the serial port, audio and MIDI, each only while another command channel stays. `keep` is never
// dropped (what was just asked for). The result fits, or is the best it can do (Endpoints::fits says).
struct Fitted {
    Classes classes;
    Classes dropped;  // what gave way
};

inline constexpr Fitted Fit(const Classes& wanted, const Classes& keep)
{
    Fitted out{wanted, Classes{}};
    Classes& c = out.classes;
    const auto commandWithout = [&](bool midi, bool hid, bool serial) {
        return (c.midi && !midi) || (c.hid && !hid) || c.webusb || (c.serial && !serial);
    };
    if (InEndpoints(c) > kInEndpoints && c.audio && c.playback && c.feedback && !keep.feedback)
        c.feedback = false, out.dropped.feedback = true;
    if (InEndpoints(c) > kInEndpoints && c.hid && !keep.hid && commandWithout(false, true, false))
        c.hid = false, out.dropped.hid = true;
    if (InEndpoints(c) > kInEndpoints && c.serial && !keep.serial && commandWithout(false, false, true))
        c.serial = false, out.dropped.serial = true;
    if (InEndpoints(c) > kInEndpoints && c.audio && !keep.audio)
        c.audio = false, c.feedback = false, out.dropped.audio = true;
    if (InEndpoints(c) > kInEndpoints && c.midi && !keep.midi && commandWithout(true, false, false))
        c.midi = false, out.dropped.midi = true;
    return out;
}

// Endpoint addresses (0: the class is not there). The order keeps the device without the new classes exactly
// as it was (MIDI 1, playback 2 with its feedback, recording 3, HID 4).
struct Endpoints {
    uint8_t midiOut = 0, midiIn = 0;
    uint8_t audioOut = 0, audioFeedback = 0, audioIn = 0;
    uint8_t hidOut = 0, hidIn = 0;
    uint8_t serialNotify = 0, serialOut = 0, serialIn = 0;
    uint8_t driveOut = 0, driveIn = 0;
    uint8_t cameraIn = 0;
    int inCount = 0;
    bool fits = false;
};

inline constexpr Endpoints Allocate(const Classes& classes)
{
    Endpoints endpoints;
    uint8_t next = 1;  // the next IN endpoint number
    if (classes.midi)
    {
        endpoints.midiOut = next;
        endpoints.midiIn = static_cast<uint8_t>(0x80 | next);
        ++next;
    }
    if (classes.audio)
    {
        if (classes.playback && classes.feedback)
        {
            // A feedback endpoint has the number of the data endpoint it serves (USB 2.0, 9.6.6).
            endpoints.audioOut = next;
            endpoints.audioFeedback = static_cast<uint8_t>(0x80 | next);
            ++next;
        }
        else if (classes.playback)
            endpoints.audioOut = kAdaptiveAudioOut;
        endpoints.audioIn = static_cast<uint8_t>(0x80 | next);
        ++next;
    }
    if (classes.hid)
    {
        endpoints.hidOut = next;
        endpoints.hidIn = static_cast<uint8_t>(0x80 | next);
        ++next;
    }
    if (classes.serial)
    {
        endpoints.serialNotify = static_cast<uint8_t>(0x80 | next);
        ++next;
        endpoints.serialOut = next;
        endpoints.serialIn = static_cast<uint8_t>(0x80 | next);
        ++next;
    }
    if (classes.drive)
    {
        endpoints.driveOut = next;
        endpoints.driveIn = static_cast<uint8_t>(0x80 | next);
        ++next;
    }
    if (classes.camera)
    {
        endpoints.cameraIn = static_cast<uint8_t>(0x80 | next);
        ++next;
    }
    endpoints.inCount = next - 1;
    endpoints.fits = endpoints.inCount <= kInEndpoints;
    return endpoints;
}

// ---- Identity --------------------------------------------------------------------------------------------

// The serial number: the chip's MAC as USB-Serial-JTAG gives it (and so the ROM's download mode and the 203
// Launcher, which run on it): "70:AF:09:BC:EB:30". A computer finds the same chip in every mode by it. `out`
// holds 18 bytes.
inline void SerialNumber(const uint8_t mac[6], char out[18])
{
    static constexpr char kDigits[] = "0123456789ABCDEF";
    for (int index = 0; index < 6; ++index)
    {
        out[3 * index] = kDigits[mac[index] >> 4];
        out[3 * index + 1] = kDigits[mac[index] & 15];
        out[3 * index + 2] = index == 5 ? '\0' : ':';
    }
}

// ---- The serial port's lines ------------------------------------------------------------------------------

inline constexpr std::size_t kMaxLine = 160;  // longer lines are dropped whole

// Collects a command line from what the computer sends: "\n" ends it, "\r" is ignored, control characters
// are dropped, a line too long is thrown away up to its end. Push returns true when `line` holds one.
class LineReader {
  public:
    bool Push(char c)
    {
        if (c == '\r') return false;
        if (c == '\n')
        {
            const bool whole = !overflow_ && length_ > 0;
            line_[whole ? length_ : 0] = '\0';
            ready_ = length_;
            length_ = 0;
            overflow_ = false;
            return whole;
        }
        if (static_cast<unsigned char>(c) < 0x20 && c != '\t') return false;
        if (length_ + 1 >= kMaxLine)
        {
            overflow_ = true;
            return false;
        }
        line_[length_++] = c == '\t' ? ' ' : c;
        return false;
    }
    const char* Line() const { return line_; }

  private:
    char line_[kMaxLine] = {};
    std::size_t length_ = 0;
    std::size_t ready_ = 0;
    bool overflow_ = false;
};

// The command words of a line: lower case compared, spaces around ignored. Equal(line, "log on").
inline bool SameWords(const char* line, const char* words)
{
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    while (*line == ' ') ++line;
    while (*words != '\0')
    {
        if (*words == ' ')
        {
            if (*line != ' ') return false;
            while (*line == ' ') ++line;
            ++words;
            continue;
        }
        if (lower(*line) != *words) return false;
        ++line;
        ++words;
    }
    while (*line == ' ') ++line;
    return *line == '\0';
}

// "sys 44" / "app 41 42": the word, then hex bytes (spaces between them or not). Returns the number of bytes
// (at most `capacity`), or -1 when the line is not that word or its hex is not whole bytes.
inline int HexArgument(const char* line, const char* word, uint8_t* out, std::size_t capacity)
{
    while (*line == ' ') ++line;
    for (; *word != '\0'; ++word, ++line)
    {
        const char c = *line >= 'A' && *line <= 'Z' ? static_cast<char>(*line - 'A' + 'a') : *line;
        if (c != *word) return -1;
    }
    if (*line != ' ' && *line != '\0') return -1;
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::size_t count = 0;
    int high = -1;
    for (; *line != '\0'; ++line)
    {
        if (*line == ' ') continue;
        const int value = digit(*line);
        if (value < 0) return -1;
        if (high < 0)
        {
            high = value;
            continue;
        }
        if (count == capacity) return -1;
        out[count++] = static_cast<uint8_t>(high << 4 | value);
        high = -1;
    }
    return high < 0 ? static_cast<int>(count) : -1;
}

// "<word> <hex of the bytes>\n" into `out` (room for word + 1 + 2 * length + 2). Returns the length written.
inline std::size_t HexLine(const char* word, const uint8_t* bytes, std::size_t length, char* out)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::size_t at = 0;
    for (; *word != '\0'; ++word) out[at++] = *word;
    out[at++] = ' ';
    for (std::size_t index = 0; index < length; ++index)
    {
        out[at++] = kDigits[bytes[index] >> 4];
        out[at++] = kDigits[bytes[index] & 15];
    }
    out[at++] = '\n';
    out[at] = '\0';
    return at;
}

// ---- The reset into the ROM's download mode, as esptool and Arduino ask a serial port for it ---------------
//
// On a USB-to-serial bridge DTR drives IO0 and RTS drives EN (inverted); esptool restarts the chip into its
// download mode with RTS asserted while DTR is not (EN low: in reset), then DTR asserted while RTS is not (IO0
// low as EN goes high), then both released: ClassicReset sets them one after the other ((0,1) (1,1) (1,0)
// (0,0)), UnixTightReset together ((0,0) (1,1) (0,1) (1,0) (0,0)). Arduino opens the port at 1200 baud and
// closes it ("1200 baud touch"). A port opened or closed plainly sets both together, or neither: never this
// sequence. Repeated states (Windows sends one again to make the other line go out) change nothing.
class ResetDetector {
  public:
    // Each SET_CONTROL_LINE_STATE. True: restart into the download mode now.
    bool Lines(bool dtr, bool rts)
    {
        const int state = (dtr ? 2 : 0) | (rts ? 1 : 0);
        if (state == last_) return false;
        last_ = state;
        if (baud1200_ && !dtr) return true;  // the 1200 baud touch: the port closes
        switch (state)
        {
            case 1:  // RTS only: EN low, the chip held in reset
                step_ = 1;
                return false;
            case 3:  // both: ClassicReset passes through it on its way out of reset
                if (step_ != 1) step_ = 0;
                return false;
            case 2:  // DTR only: IO0 low as EN goes high
                step_ = step_ == 1 ? 2 : 0;
                return false;
            default:  // neither: IO0 released, the end of the sequence
            {
                const bool reset = step_ == 2;
                step_ = 0;
                return reset;
            }
        }
    }
    // Each SET_LINE_CODING.
    void Baud(uint32_t bitsPerSecond) { baud1200_ = bitsPerSecond == 1200; }

  private:
    int last_ = -1;
    int step_ = 0;
    bool baud1200_ = false;
};

// ---- Playback without a feedback endpoint ----------------------------------------------------------------
//
// Adaptive playback: the computer sends 44.1 frames a millisecond by its own clock and the device plays them by
// its crystal, so the FIFO between them slowly fills or empties (100 ppm: 4.4 frames a second). Each read takes
// what was asked; the average of what the FIFO held at each read decides whether one frame is dropped (it rose
// above the band) or the last one played twice (it fell below), at most one each read: a frame in 10,000 or
// less, not heard as a click. The band sits `margin` frames above the read, so a read always finds its frames.
class RateMatch {
  public:
    explicit RateMatch(int marginFrames = 96, int bandFrames = 48) : margin_(marginFrames), band_(bandFrames) {}
    void Reset() { average_ = -1; }
    // Before a read of `asked` frames with `available` in the FIFO: +1 drop one, -1 repeat one, 0 neither.
    int Decide(int available, int asked)
    {
        if (average_ < 0) average_ = available << 4;
        average_ += available - (average_ >> 4);  // about the last 16 reads (90 ms of 256-frame blocks)
        const int fill = average_ >> 4;
        const int target = asked + margin_;
        if (fill > target + band_ && available > asked)
        {
            average_ -= 1 << 4;
            return 1;
        }
        if (fill < target - band_ && asked > 1)
        {
            average_ += 1 << 4;
            return -1;
        }
        return 0;
    }

  private:
    int margin_;
    int band_;
    int average_ = -1;  // the FIFO's fill at a read, in frames, times 16
};

}  // namespace CardenzaHAL::USB::Logic
