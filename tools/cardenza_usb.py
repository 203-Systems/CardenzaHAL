"""A Cardenza on this computer's USB, from the command line: the host side of CardenzaHAL::USB, for any
firmware built on the HAL.

Talks only to devices with 203 Systems' vendor ID (0x0203): the HID system channel (report 0xCB), the
firmware's app channel (report 0xFF, or SysEx F0 23 7F <bytes> F7), and MIDI ports and audio devices
whose name contains "Cardenza".

usage: python3 <HAL>/tools/cardenza_usb.py list
       python3 <HAL>/tools/cardenza_usb.py command identify|reboot|launcher|download
       python3 <HAL>/tools/cardenza_usb.py console <seconds>
       python3 <HAL>/tools/cardenza_usb.py app <text> [seconds]

  list      the board's HID, MIDI and audio devices
  command   restart it (reboot), into the 203 Launcher (launcher) or into ROM download mode for esptool
            (download: the way to flash a board whose firmware runs USB and so has no serial port), or ask
            who it is (identify); over HID, else over SysEx F0 23 <code> F7
  console   its console (stdout, stderr, the log) for that many seconds, over HID
  app       sends <text> to the firmware's app channel and prints what it answers (what that means is
            the firmware's business: CardenzaHAL::USB::SetAppReportHandler / SendAppReport)

A firmware's own tool can import this file (importlib, from the HAL's path) and use open_hid, send,
receive, midi, sysex_command, command, app_exchange and split_sysex.

Needs: pip install hidapi (HID), sounddevice (audio devices in list). MIDI goes through macOS CoreMIDI
(no package); elsewhere the MIDI parts say so and the HID parts still work.
"""

import collections
import contextlib
import os
import re
import struct
import sys
import threading
import time

VENDOR_ID = 0x0203
REPORT_SYSTEM = 0xCB
REPORT_APP = 0xFF
REPORT_SIZE = 63
COMMANDS = {"launcher": 0x40, "reboot": 0x41, "download": 0x42, "console": 0x43, "identify": 0x44}
NAME = "Cardenza"


def hid_devices():
    import hid
    return [d for d in hid.enumerate(VENDOR_ID, 0) if d.get("usage_page") == 0xFF00]


def open_hid():
    try:
        import hid
    except ImportError:
        return None, "hidapi not installed (pip install hidapi)"
    devices = hid_devices()
    if not devices:
        return None, "no Cardenza HID device"
    device = hid.device()
    try:
        device.open_path(devices[0]["path"])
    except OSError as error:
        return None, "HID open failed: %s" % error
    device.set_nonblocking(False)
    return device, None


def send(device, report_id, payload):
    data = bytes(payload[:REPORT_SIZE]).ljust(REPORT_SIZE, b"\0")
    device.write(bytes([report_id]) + data)


def receive(device, report_id, seconds):
    end = time.time() + seconds
    while time.time() < end:
        data = device.read(REPORT_SIZE + 1, 100)
        if data and data[0] == report_id:
            return bytes(data[1:])
    return None


class CoreMidi:
    """The Cardenza's MIDI ports through macOS CoreMIDI (ctypes: no package to build)."""

    def __init__(self):
        self._closed = True
        import ctypes
        import ctypes.util
        self.c = ctypes
        cm = ctypes.cdll.LoadLibrary(ctypes.util.find_library("CoreMIDI"))
        cf = ctypes.cdll.LoadLibrary(ctypes.util.find_library("CoreFoundation"))
        u32, vp = ctypes.c_uint32, ctypes.c_void_p
        cf.CFStringCreateWithCString.restype = vp
        cf.CFStringCreateWithCString.argtypes = [vp, ctypes.c_char_p, u32]
        cf.CFStringGetCString.restype = ctypes.c_bool
        cf.CFStringGetCString.argtypes = [vp, ctypes.c_char_p, ctypes.c_long, u32]
        cf.CFRelease.argtypes = [vp]
        cf.CFRelease.restype = None
        self.read_proc_type = ctypes.CFUNCTYPE(None, vp, vp, vp)
        cm.MIDIClientCreate.argtypes = [vp, vp, vp, ctypes.POINTER(u32)]
        cm.MIDIOutputPortCreate.argtypes = [u32, vp, ctypes.POINTER(u32)]
        cm.MIDIInputPortCreate.argtypes = [u32, vp, self.read_proc_type, vp, ctypes.POINTER(u32)]
        cm.MIDIPortConnectSource.argtypes = [u32, u32, vp]
        cm.MIDIPortDisconnectSource.argtypes = [u32, u32]
        cm.MIDIPortDispose.argtypes = [u32]
        cm.MIDIGetNumberOfSources.restype = ctypes.c_ulong
        cm.MIDIGetNumberOfDestinations.restype = ctypes.c_ulong
        cm.MIDIGetSource.restype = u32
        cm.MIDIGetSource.argtypes = [ctypes.c_ulong]
        cm.MIDIGetDestination.restype = u32
        cm.MIDIGetDestination.argtypes = [ctypes.c_ulong]
        cm.MIDIObjectGetStringProperty.argtypes = [u32, vp, ctypes.POINTER(vp)]
        cm.MIDISend.argtypes = [u32, u32, vp]
        for name in ("MIDIClientCreate", "MIDIOutputPortCreate", "MIDIInputPortCreate",
                     "MIDIPortConnectSource", "MIDIPortDisconnectSource", "MIDIPortDispose",
                     "MIDIObjectGetStringProperty", "MIDISend"):
            getattr(cm, name).restype = ctypes.c_int32
        self.cm, self.cf = cm, cf
        self.lock = threading.Lock()
        self.received = collections.deque()
        self.client, self.out, self.inp = u32(), u32(), u32()
        self.source = self.destination = None
        self.proc = self.read_proc_type(self._read)  # kept alive as long as the port
        self._closed = False
        try:
            with self._string("cardenza-hal-usb") as name:
                self._check(cm.MIDIClientCreate(name, None, None, ctypes.byref(self.client)), "MIDIClientCreate")
            # CoreMIDI initializes property constants when its first client is created.
            self.name_property = vp.in_dll(cm, "kMIDIPropertyDisplayName").value
            if not self.name_property:
                raise OSError("CoreMIDI display-name property is not initialized")
            with self._string("out") as name:
                self._check(cm.MIDIOutputPortCreate(self.client, name, ctypes.byref(self.out)), "MIDIOutputPortCreate")
            with self._string("in") as name:
                self._check(cm.MIDIInputPortCreate(self.client, name, self.proc, None, ctypes.byref(self.inp)), "MIDIInputPortCreate")
            self.destination = self._find(cm.MIDIGetNumberOfDestinations, cm.MIDIGetDestination)
            self.source = self._find(cm.MIDIGetNumberOfSources, cm.MIDIGetSource)
            if self.source is not None:
                self._check(cm.MIDIPortConnectSource(self.inp, self.source, None), "MIDIPortConnectSource")
        except BaseException:
            self.close()
            raise

    @staticmethod
    def _check(status, operation):
        if status:
            raise OSError("%s: OSStatus %s" % (operation, status))

    @contextlib.contextmanager
    def _string(self, text):
        value = self.cf.CFStringCreateWithCString(None, text.encode(), 0x08000100)
        if not value:
            raise OSError("CFStringCreateWithCString failed")
        try:
            yield value
        finally:
            self.cf.CFRelease(value)

    def close(self):
        if self._closed:
            return
        self._closed = True
        if self.inp.value:
            if self.source is not None:
                self.cm.MIDIPortDisconnectSource(self.inp, self.source)
            self.cm.MIDIPortDispose(self.inp)
            self.inp.value = 0
        if self.out.value:
            self.cm.MIDIPortDispose(self.out)
            self.out.value = 0
        # CoreMIDI has stopped using the callback; only now release its Python owner.
        self.proc = None
        # Clients are process-owned and automatically disposed at process exit. Apple
        # discourages disposing the last client before creating another in the same process.

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def name(self, endpoint):
        value = self.c.c_void_p()
        if self.cm.MIDIObjectGetStringProperty(endpoint, self.name_property, self.c.byref(value)) != 0 or not value.value:
            return ""
        buffer = self.c.create_string_buffer(256)
        self.cf.CFStringGetCString(value, buffer, 256, 0x08000100)
        self.cf.CFRelease(value)
        return buffer.value.decode(errors="replace")

    def _find(self, count, get):
        for index in range(count()):
            endpoint = get(index)
            if NAME in self.name(endpoint):
                return endpoint
        return None

    def _read(self, packet_list, _proc_ref, _source_ref):
        if self._closed or not packet_list:
            return
        # MIDIPacketList: UInt32 count, then packets of UInt64 time, UInt16 length, data, 4-byte aligned.
        address = packet_list
        count = self.c.c_uint32.from_address(address).value
        packet = address + 4
        for _ in range(count):
            length = self.c.c_uint16.from_address(packet + 8).value
            data = self.c.string_at(packet + 10, length)
            with self.lock:
                self.received.extend(data)
            packet = (packet + 10 + length + 3) & ~3

    def send(self, data):
        if self._closed or self.destination is None:
            return False
        data = bytes(data)
        packet_list = self.c.create_string_buffer(struct.pack("<IQH", 1, 0, len(data)) + data)
        return self.cm.MIDISend(self.out, self.destination, packet_list) == 0

    def take(self):
        with self.lock:
            data = bytes(self.received)
            self.received.clear()
        return data


def midi():
    if sys.platform != "darwin":
        return None, "MIDI here only on macOS (CoreMIDI)"
    try:
        port = CoreMidi()
    except (OSError, AttributeError, ValueError) as error:
        return None, "CoreMIDI: %s" % error
    if port.destination is None or port.source is None:
        port.close()
        return None, "no Cardenza MIDI port"
    return port, None


def sysex_command(code, seconds=1.5):
    port, error = midi()
    if port is None:
        return None, error
    try:
        port.take()
        if not port.send([0xF0, 0x23, code, 0xF7]):
            return None, "CoreMIDI send failed"
        end = time.monotonic() + seconds
        buffered = b""
        while time.monotonic() < end:
            buffered += port.take()
            start = buffered.find(bytes([0xF0, 0x24]))
            if start >= 0:
                stop = buffered.find(b"\xf7", start)
                if stop > start:
                    return buffered[start + 2:stop], None
            time.sleep(0.01)
        return None, "no SysEx answer"
    finally:
        port.close()



def command(name):
    code = COMMANDS[name]
    device, error = open_hid()
    if device is not None:
        send(device, REPORT_SYSTEM, [code])
        answer = receive(device, REPORT_SYSTEM, 1.5)
        device.close()
        if answer is not None:
            return "hid", answer
        error = "no HID answer"
    answer, sysex_error = sysex_command(code)
    if answer is not None:
        return "sysex", answer
    raise SystemExit("usb: %s; %s" % (error, sysex_error))



def console(seconds):
    device, error = open_hid()
    if device is None:
        raise SystemExit("usb: " + error)
    send(device, REPORT_SYSTEM, [COMMANDS["console"], 1])
    end = time.time() + seconds
    try:
        while time.time() < end:
            data = device.read(REPORT_SIZE + 1, 100)
            if data and data[0] == REPORT_SYSTEM and data[1] == (COMMANDS["console"] | 0x80):
                sys.stdout.write(bytes(data[3:3 + data[2]]).decode(errors="replace"))
                sys.stdout.flush()
    finally:
        send(device, REPORT_SYSTEM, [COMMANDS["console"], 0])
        device.close()


def list_all():
    try:
        for d in hid_devices():
            print("hid: %04x:%04x %s %s serial=%s interface=%s" % (d["vendor_id"], d["product_id"], d.get("manufacturer_string"), d.get("product_string"), d.get("serial_number"), d.get("interface_number")))
    except ImportError:
        print("hid: hidapi not installed (pip install hidapi)")
    port, error = midi()
    try:
        print("midi: %s" % ("out=%s in=%s" % (port.name(port.destination), port.name(port.source)) if port else error))
    finally:
        if port:
            port.close()
    try:
        import sounddevice
        for index, d in enumerate(sounddevice.query_devices()):
            if NAME in d["name"]:
                print("audio: #%d %s in=%d out=%d rate=%d" % (index, d["name"], d["max_input_channels"], d["max_output_channels"], d["default_samplerate"]))
    except ImportError:
        print("audio: sounddevice not installed (pip install sounddevice numpy)")



def split_sysex(buffered, prefix):
    """The messages in buffered that start with prefix and end with F7, and what is left after them."""
    messages = []
    while True:
        start = buffered.find(prefix)
        if start < 0:
            return messages, buffered[-(len(prefix) - 1):] if len(prefix) > 1 else b""
        stop = buffered.find(b"\xf7", start + len(prefix))
        if stop < 0:
            return messages, buffered[start:]
        messages.append(buffered[start + len(prefix):stop])
        buffered = buffered[stop + 1:]


def app_exchange(payload, seconds=1.5):
    """Sends payload (bytes, at most 63) to the firmware's app channel and returns ([answers], None), or
    (None, why not): over HID report 0xFF, else over SysEx F0 23 7F <7-bit bytes> F7 (answers F0 24 7F ... F7)."""
    payload = bytes(payload)
    if len(payload) > REPORT_SIZE:
        return None, "at most %d bytes" % REPORT_SIZE
    device, error = open_hid()
    if device is not None:
        try:
            send(device, REPORT_APP, payload)
            answers = []
            end = time.time() + seconds
            while time.time() < end:
                data = device.read(REPORT_SIZE + 1, 100)
                if data and data[0] == REPORT_APP:
                    answers.append(bytes(data[1:]).rstrip(b"\0"))
            return answers, None
        finally:
            device.close()
    if any(byte > 0x7F for byte in payload):
        return None, "%s; SysEx carries 7-bit bytes only" % error
    port, midi_error = midi()
    if port is None:
        return None, "%s; %s" % (error, midi_error)
    try:
        port.take()
        if not port.send([0xF0, 0x23, 0x7F] + list(payload) + [0xF7]):
            return None, "CoreMIDI send failed"
        answers, buffered = [], b""
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            found, buffered = split_sysex(buffered + port.take(), bytes([0xF0, 0x24, 0x7F]))
            answers.extend(found)
            time.sleep(0.01)
        return answers, None
    finally:
        port.close()


def watchdog(seconds):
    """Ends the process when an action runs too long (a USB call that never returns), wherever it blocks."""
    def expire():
        print("usb: timed out after %d s" % seconds, flush=True)
        os._exit(124)
    timer = threading.Timer(seconds, expire)
    timer.daemon = True
    timer.start()


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    action = sys.argv[1]
    if action == "list":
        watchdog(30)
        list_all()
    elif action == "command" and len(sys.argv) == 3 and sys.argv[2] in COMMANDS and sys.argv[2] != "console":
        watchdog(20)
        transport, answer = command(sys.argv[2])
        print("%s answer: %s" % (transport, answer.rstrip(b"\0").hex()))
    elif action == "console" and len(sys.argv) == 3:
        watchdog(float(sys.argv[2]) + 15)
        console(float(sys.argv[2]))
    elif action == "app" and len(sys.argv) in (3, 4):
        seconds = float(sys.argv[3]) if len(sys.argv) == 4 else 1.5
        watchdog(seconds + 15)
        answers, error = app_exchange(sys.argv[2].encode(), seconds)
        if answers is None:
            raise SystemExit("usb: " + error)
        for answer in answers:
            print(answer.decode(errors="replace"))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
