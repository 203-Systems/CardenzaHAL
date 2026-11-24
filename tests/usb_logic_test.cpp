// usb/usb_logic.h: the classes a device presents within the ESP32-S3's four IN endpoints, what gives way, the
// endpoint numbers, the serial number, the serial port's lines, esptool's reset, and the adaptive playback's
// rate matching; system/crash_loop_logic.h: back to the Launcher after early crashes in a row.
#include "system/crash_loop_logic.h"
#include "usb/usb_logic.h"

#include <cassert>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>

using namespace CardenzaHAL::USB::Logic;

namespace {

Classes Make(bool midi, bool audio, bool feedback, bool hid, bool webusb, bool serial, bool drive = false, bool camera = false)
{
    Classes classes;
    classes.midi = midi;
    classes.audio = audio;
    classes.playback = audio;
    classes.feedback = feedback;
    classes.hid = hid;
    classes.webusb = webusb;
    classes.serial = serial;
    classes.drive = drive;
    classes.camera = camera;
    return classes;
}

void BudgetTests()
{
    // The default: MIDI, audio with its feedback endpoint, HID and WebUSB: all four IN endpoints, numbered as
    // before the serial port existed (MIDI 1, playback 2 + feedback 0x82, recording 0x83, HID 4).
    const Classes all = Make(true, true, true, true, true, false);
    assert(InEndpoints(all) == 4);
    const Endpoints endpoints = Allocate(all);
    assert(endpoints.fits && endpoints.inCount == 4);
    assert(endpoints.midiOut == 0x01 && endpoints.midiIn == 0x81);
    assert(endpoints.audioOut == 0x02 && endpoints.audioFeedback == 0x82 && endpoints.audioIn == 0x83);
    assert(endpoints.hidOut == 0x04 && endpoints.hidIn == 0x84);
    assert(endpoints.serialNotify == 0 && endpoints.driveIn == 0 && endpoints.cameraIn == 0);

    // The serial port takes two: MIDI, audio without feedback (adaptive) and the serial port fit.
    const Endpoints serial = Allocate(Make(true, true, false, false, true, true));
    assert(serial.fits && serial.inCount == 4);
    assert(serial.audioOut == kAdaptiveAudioOut && serial.audioFeedback == 0 && serial.audioIn == 0x82);
    assert(serial.serialNotify == 0x83 && serial.serialOut == 0x04 && serial.serialIn == 0x84);
    assert(!Allocate(Make(true, true, true, true, true, true)).fits);

    // IN endpoint numbers stay 1 to 4 (their TX FIFOs) whenever the classes fit; OUT numbers up to 6.
    for (int mask = 0; mask < 256; ++mask)
    {
        Classes classes = Make(mask & 1, mask & 2, mask & 4, mask & 8, mask & 16, mask & 32, mask & 64, mask & 128);
        classes.feedback = classes.feedback && classes.audio;
        const Endpoints e = Allocate(classes);
        assert(e.inCount == InEndpoints(classes));
        if (!e.fits) continue;
        for (uint8_t in : {e.midiIn, e.audioFeedback, e.audioIn, e.hidIn, e.serialNotify, e.serialIn, e.driveIn, e.cameraIn})
            assert(in == 0 || ((in & 0x80) && (in & 0x7F) >= 1 && (in & 0x7F) <= 4));
        for (uint8_t out : {e.midiOut, e.audioOut, e.hidOut, e.serialOut, e.driveOut}) assert(out <= 6 && !(out & 0x80));
        // No two endpoints of a direction share a number.
        const uint8_t ins[] = {e.midiIn, e.audioFeedback, e.audioIn, e.hidIn, e.serialNotify, e.serialIn, e.driveIn, e.cameraIn};
        for (std::size_t a = 0; a < 8; ++a)
            for (std::size_t b = a + 1; b < 8; ++b) assert(ins[a] == 0 || ins[a] != ins[b]);
        const uint8_t outs[] = {e.midiOut, e.audioOut, e.hidOut, e.serialOut, e.driveOut};
        for (std::size_t a = 0; a < 5; ++a)
            for (std::size_t b = a + 1; b < 5; ++b) assert(outs[a] == 0 || outs[a] != outs[b]);
    }

    // Recording only: one IN endpoint, no OUT, no feedback.
    Classes record = Make(true, true, true, true, true, false);
    record.playback = false;
    assert(InEndpoints(record) == 3);
    const Endpoints recordEp = Allocate(record);
    assert(recordEp.audioOut == 0 && recordEp.audioFeedback == 0 && recordEp.audioIn == 0x82);
}

void CommandChannelTests()
{
    const Classes built = Built(true, true, true, true, true, false, true, true);
    assert(!built.serial);
    // A build with no MIDI, HID or WebUSB has the serial port, and a device always keeps one channel.
    const Classes audioOnly = Built(false, true, true, false, false, false, false, false);
    assert(audioOnly.serial);
    const Classes nothing = Effective(Make(false, true, true, false, false, false), audioOnly);
    assert(nothing.serial && nothing.audio && HasCommandChannel(nothing));
    // Asking for none of them keeps WebUSB (no endpoint) when it is built.
    const Classes quiet = Effective(Make(false, true, true, false, false, false), built);
    assert(quiet.webusb && !quiet.midi && !quiet.hid && HasCommandChannel(quiet));
    // Without WebUSB: the serial port if built, else MIDI.
    const Classes noWeb = Built(true, true, true, true, false, true, false, false);
    const Classes kept = Effective(Make(false, true, false, false, false, false), noWeb);
    assert(kept.serial && HasCommandChannel(kept));
    const Classes noWebNoSerial = Built(true, true, true, true, false, false, false, false);
    const Classes midi = Effective(Make(false, true, false, false, false, false), noWebNoSerial);
    assert(midi.midi && !midi.hid);
    // What is not built is not presented.
    const Classes asked = Effective(Make(true, true, true, true, true, true, true, true), Built(true, false, false, true, true, false, false, false));
    assert(asked.midi && !asked.audio && !asked.feedback && asked.hid && asked.webusb && !asked.serial && !asked.drive && !asked.camera);
}

void FitTests()
{
    // The camera in CardenzaSynth's set: the feedback endpoint gives way (the playback becomes adaptive).
    Classes keep;
    keep.camera = true;
    Fitted fitted = Fit(Make(true, true, true, true, true, false, false, true), keep);
    assert(fitted.dropped.feedback && !fitted.dropped.hid && fitted.classes.camera && fitted.classes.hid);
    assert(Allocate(fitted.classes).fits);
    // Camera and drive: HID goes too, MIDI and WebUSB stay.
    keep.drive = true;
    fitted = Fit(Make(true, true, true, true, true, false, true, true), keep);
    assert(fitted.dropped.feedback && fitted.dropped.hid && fitted.classes.midi && fitted.classes.webusb);
    assert(Allocate(fitted.classes).fits && fitted.classes.drive && fitted.classes.camera);
    // HID is the only command channel: the feedback endpoint is enough.
    Classes onlyHid = Make(false, true, true, true, false, false, true, true);
    fitted = Fit(onlyHid, keep);
    assert(fitted.classes.hid && fitted.dropped.feedback && !fitted.dropped.audio && Allocate(fitted.classes).fits);
    // ... and when the feedback must stay too, HID still stays (the only command channel) and audio goes.
    Classes keepFeedback = keep;
    keepFeedback.feedback = true;
    fitted = Fit(onlyHid, keepFeedback);
    assert(fitted.classes.hid && !fitted.dropped.hid && fitted.dropped.audio && Allocate(fitted.classes).fits);
    // The USB network (two IN endpoints) in the default set: the feedback endpoint and HID give way, MIDI, audio
    // (adaptive) and WebUSB stay; its endpoints are numbered within 1-4.
    Classes keepNetwork;
    keepNetwork.network = true;
    Classes withNetwork = Make(true, true, true, true, true, false);
    withNetwork.network = true;
    fitted = Fit(withNetwork, keepNetwork);
    assert(fitted.dropped.feedback && fitted.dropped.hid && fitted.classes.midi && fitted.classes.audio && fitted.classes.webusb);
    const Endpoints net = Allocate(fitted.classes);
    assert(net.fits && net.inCount == 4 && net.networkNotify == 0x83 && net.networkIn == 0x84 && net.networkOut == 0x04);
    // What fits is left alone.
    fitted = Fit(Make(true, true, true, true, true, false), Classes{});
    assert(!fitted.dropped.feedback && !fitted.dropped.hid && fitted.classes == Make(true, true, true, true, true, false));
}

void LandingTests()
{
    assert(LandingPage(0, false));   // the first time
    assert(!LandingPage(0, true));   // seen: not again
    assert(LandingPage(1, true) && LandingPage(1, false));    // always
    assert(!LandingPage(2, false) && !LandingPage(2, true));  // never
}

void SerialNumberTests()
{
    const uint8_t mac[6] = {0x70, 0xAF, 0x09, 0xBC, 0xEB, 0x30};
    char text[18];
    SerialNumber(mac, text);
    assert(std::string(text) == "70:AF:09:BC:EB:30");
}

void LineTests()
{
    LineReader reader;
    std::string got;
    const auto feed = [&](const char* text) {
        got.clear();
        for (; *text; ++text)
            if (reader.Push(*text)) got = reader.Line();
    };
    feed("version\r\n");
    assert(got == "version");
    feed("\n\n");
    assert(got.empty());  // empty lines are nothing
    std::string longLine(kMaxLine + 10, 'a');
    feed((longLine + "\n").c_str());
    assert(got.empty());  // too long: dropped whole
    feed("info\n");
    assert(got == "info");  // and the next line is whole again
    feed("in\x01" "fo\n");
    assert(got == "info");  // control characters dropped

    assert(SameWords("  Version ", "version"));
    assert(SameWords("log   on", "log on"));
    assert(!SameWords("log", "log on"));
    assert(!SameWords("versions", "version"));

    uint8_t bytes[63];
    assert(HexArgument("sys 44", "sys", bytes, sizeof(bytes)) == 1 && bytes[0] == 0x44);
    assert(HexArgument("SYS 45 01 01 00", "sys", bytes, sizeof(bytes)) == 4 && bytes[1] == 0x01 && bytes[3] == 0x00);
    assert(HexArgument("sys 450101", "sys", bytes, sizeof(bytes)) == 3);
    assert(HexArgument("sys 4", "sys", bytes, sizeof(bytes)) == -1);   // half a byte
    assert(HexArgument("sys 4g", "sys", bytes, sizeof(bytes)) == -1);  // not hex
    assert(HexArgument("system", "sys", bytes, sizeof(bytes)) == -1);  // another word
    assert(HexArgument("app", "app", bytes, sizeof(bytes)) == 0);
    std::string many = "sys";
    for (int index = 0; index < 64; ++index) many += " 00";
    assert(HexArgument(many.c_str(), "sys", bytes, sizeof(bytes)) == -1);  // more than a report

    char line[8 + 2 * 63 + 2];
    const uint8_t report[3] = {0xC4, 0x04, 0x43};
    const std::size_t size = HexLine("sys", report, 3, line);
    assert(std::string(line, size) == "sys c40443\n");
    // A line goes back in as the same bytes.
    line[size - 1] = '\0';
    assert(HexArgument(line, "sys", bytes, sizeof(bytes)) == 3 && std::memcmp(bytes, report, 3) == 0);
}

// (DTR, RTS) as a host sends them.
bool Run(ResetDetector& detector, std::initializer_list<std::pair<int, int>> states)
{
    bool reset = false;
    for (const auto& [dtr, rts] : states) reset = detector.Lines(dtr, rts) || reset;
    return reset;
}

void ResetTests()
{
    {
        ResetDetector detector;  // esptool ClassicReset after a port opened by Linux (both raised)
        assert(Run(detector, {{1, 1}, {0, 1}, {1, 1}, {1, 0}, {0, 0}}));
    }
    {
        ResetDetector detector;  // ClassicReset with Windows' repeated states (usbser.sys work-around)
        assert(Run(detector, {{0, 0}, {0, 1}, {0, 1}, {1, 1}, {1, 0}, {1, 0}, {0, 0}}));
    }
    {
        ResetDetector detector;  // esptool UnixTightReset
        assert(Run(detector, {{0, 0}, {1, 1}, {0, 1}, {1, 0}, {0, 0}, {0, 0}}));
    }
    {
        ResetDetector detector;  // ports opened and closed, terminals, the gate's log reader: never
        assert(!Run(detector, {{1, 1}, {0, 0}, {1, 1}, {0, 0}}));
        assert(!Run(detector, {{0, 0}, {0, 0}}));
        assert(!Run(detector, {{1, 0}, {0, 0}}));        // DTR alone, released: no reset before it
        assert(!Run(detector, {{0, 1}, {0, 0}}));        // a reset, but IO0 never low
        assert(!Run(detector, {{0, 1}, {1, 1}, {0, 0}})); // ClassicReset cut short
    }
    {
        ResetDetector detector;  // Arduino's 1200 baud touch: opened at 1200, then closed
        detector.Baud(1200);
        assert(!detector.Lines(1, 1));
        assert(detector.Lines(0, 1));
    }
    {
        ResetDetector detector;  // 1200 baud and then another rate: nothing
        detector.Baud(1200);
        detector.Baud(115200);
        assert(!Run(detector, {{1, 1}, {0, 0}}));
    }
}

// A device reading 256-frame blocks from a host that sends `perRead` frames per block on average (as a
// fraction: `extra` frames more every `every` reads). Returns the drops minus the repeats; the FIFO never runs
// dry and never overflows its 512 frames.
int Simulate(int extra, int every, int reads, int* lowest = nullptr, int* highest = nullptr)
{
    RateMatch match;
    int remainder = 96;  // left after a read
    int net = 0, low = 1 << 20, high = 0;
    for (int read = 0; read < reads; ++read)
    {
        int incoming = 256;
        if (every != 0 && read % every == 0) incoming += extra;
        const int available = remainder + incoming;
        const int adjust = match.Decide(available, 256);
        assert(adjust >= -1 && adjust <= 1);
        const int taken = 256 + (adjust > 0 ? 1 : 0) - (adjust < 0 ? 1 : 0);
        assert(available >= taken);  // never dry
        remainder = available - taken;
        assert(remainder + 257 <= 512 + 64);  // within the FIFO and the next packet
        net += adjust;
        low = std::min(low, available);
        high = std::max(high, available);
    }
    if (lowest) *lowest = low;
    if (highest) *highest = high;
    return net;
}

void RateMatchTests()
{
    // On rate: nothing at all.
    assert(Simulate(0, 0, 20000) == 0);
    // 100 ppm fast (a frame more every 39 blocks): about one drop per 39 reads, the fill held in the band.
    int low = 0, high = 0;
    const int fast = Simulate(1, 39, 39000, &low, &high);
    assert(fast > 900 && fast < 1100);
    assert(low >= 256 && high <= 352 + 48 + 260);
    // 100 ppm slow.
    const int slow = Simulate(-1, 39, 39000, &low, &high);
    assert(slow < -900 && slow > -1100);
    assert(low >= 256);
    // Far off (0.4 %, a broken host clock): corrected every read, still never dry.
    assert(Simulate(1, 1, 20000) > 15000);
    assert(Simulate(-1, 1, 20000) < -15000);
}

void CrashLoopTests()
{
    using namespace CardenzaHAL::System::CrashLoop;
    State state{0, 0};  // RTC memory after a power cycle: anything
    assert(Count(state, Reset::PowerOn) == 0);
    assert(Count(state, Reset::Crash) == 1);  // no valid count yet: this crash is the first
    state = {kMagic, 0};
    uint32_t crashes = Count(state, Reset::Crash);
    assert(crashes == 1 && !GiveUp(crashes, 3));
    state.crashes = crashes;
    crashes = Count(state, Reset::Crash);
    assert(crashes == 2 && !GiveUp(crashes, 3));
    state.crashes = crashes;
    crashes = Count(state, Reset::Crash);
    assert(crashes == 3 && GiveUp(crashes, 3));
    // A software restart (the Launcher starting it, a firmware's own restart) starts again from 0.
    state.crashes = 2;
    assert(Count(state, Reset::Other) == 0);
    assert(!GiveUp(5, 0));  // 0: never
}

}  // namespace

int main()
{
    BudgetTests();
    CommandChannelTests();
    FitTests();
    SerialNumberTests();
    LandingTests();
    LineTests();
    ResetTests();
    RateMatchTests();
    CrashLoopTests();
    std::cout << "usb logic tests passed\n";
    return 0;
}
