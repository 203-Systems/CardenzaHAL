"""Offline regression checks; no pyserial, esptool or hardware required."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import serial_flasher as flasher
import register_app as register


def image(length=32, hashed=True, chip=9):
    header = bytearray(24)
    header[:2] = b'\xe9\x01'
    struct.pack_into('<H', header, 12, chip)
    header[23] = hashed
    payload = bytes(i % 256 for i in range(length))
    data = header + struct.pack('<II', 0x3C000020, length) + payload
    checksum = 0xEF
    for byte in payload:
        checksum ^= byte
    data += b'\0' * ((-(len(data) + 1)) % 16) + bytes([checksum])
    if hashed:
        data += hashlib.sha256(data).digest()
    return bytes(data)


def merged(apps):
    data = bytearray(b'\xff' * 0x40000)
    # Deliberately start with an app partition, not a data partition.
    for index, (subtype, offset, app) in enumerate(apps):
        entry = struct.pack('<HBBII16sI', 0x50AA, 0, subtype, offset, 0x10000, b'app', 0)
        pos = 0x8000 + index * 32
        data[pos:pos + 32] = entry
        data[offset:offset + len(app)] = app
    return bytes(data)


class Port:
    def __init__(self, chunks=(), short_write=False):
        self.chunks = list(chunks)
        self.written = bytearray()
        self.short_write = short_write
    in_waiting = 1
    def read(self, count):
        return self.chunks.pop(0) if self.chunks else b''
    def write(self, data):
        count = min(7, len(data)) if self.short_write else len(data)
        self.written += data[:count]
        return count
    def flush(self): pass
    def __enter__(self): return self
    def __exit__(self, *args): pass


class ImageTests(unittest.TestCase):
    def test_checksum_alignment_and_hash(self):
        # Includes a segment ending exactly at a 16-byte boundary, where
        # the upstream length formula incorrectly consumes an extra block.
        for size in range(1, 65):
            for hashed in (False, True):
                data = image(size, hashed)
                self.assertEqual(flasher.detect_app_region(data), (0, len(data), False))
                self.assertEqual(flasher.measure_esp_image_size(data + b'\xff' * 100, 0), len(data))
                self.assertEqual(flasher.measure_esp_image_size(data[:-1], 0), 0)
                corrupt = bytearray(data)
                corrupt[32] ^= 1
                self.assertEqual(flasher.measure_esp_image_size(corrupt, 0), 0)

    def test_invalid_input(self):
        for data in (b'', b'not a BIN', image(chip=0)):
            with self.assertRaises(ValueError):
                flasher.detect_app_region(data)
        bad_hash = bytearray(image())
        bad_hash[-1] ^= 1
        with self.assertRaises(ValueError):
            flasher.detect_app_region(bad_hash)

    def test_merged_selection(self):
        app = image()
        for subtype in (0, 0x10, 0x12, 0x1F, 0x20):
            data = merged([(subtype, 0x10000, app)])
            self.assertEqual(flasher.detect_app_region(data), (0x10000, len(app), True))
        for apps in ([], [(0x10, 0x10000, app), (0x11, 0x20000, app)]):
            with self.assertRaises(ValueError):
                flasher.detect_app_region(merged(apps))

    def test_check_never_opens_device(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'app.bin'
            path.write_bytes(image())
            with patch.object(sys, 'argv', ['tool', '--check', '-f', str(path)]), \
                    patch.object(flasher, 'require_serial', side_effect=AssertionError), \
                    patch.object(flasher, 'reset_board', side_effect=AssertionError), \
                    contextlib.redirect_stdout(io.StringIO()):
                flasher.main()


class ProtocolTests(unittest.TestCase):
    def test_partial_lines_and_buffered_replies(self):
        port = Port([b'noise\nLaun', b'cher 0.2.0\r\nREADY 123\n'])
        reader = flasher.LineReader(port)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertTrue(flasher.wait_for_launcher(port, reader, 0.1))
            self.assertEqual(reader.wait_for_line(lambda s: s.startswith('READY'), 0), 'READY 123')
        self.assertEqual(port.written, b'nav EscPress\nversion\n')

    def test_exact_replies(self):
        for expected in ('READY 123', 'ACK 123/123', 'OK flashed, rebooting'):
            flasher.require_reply(expected, expected)
            for bad in (None, '', 'ERR busy', expected + 'extra', 'ACK 122/123'):
                with self.assertRaises(SystemExit):
                    flasher.require_reply(bad, expected)

    def test_short_writes(self):
        port = Port(short_write=True)
        payload = image(5000)
        flasher.write_all(port, payload)
        self.assertEqual(bytes(port.written), payload)
        with patch.object(port, 'write', return_value=0):
            with self.assertRaises(ValueError):
                flasher.write_all(port, b'x')

    def test_complete_upload_and_bad_ack(self):
        payload = image(5000)
        replies = [b'Launcher 0.2.0\n', f'READY {len(payload)}\n'.encode()]
        replies += [f'ACK {min(n, len(payload))}/{len(payload)}\n'.encode()
                    for n in range(2048, len(payload) + 2048, 2048)]
        replies += [b'OK flashed, rebooting\n']
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'app.bin'
            path.write_bytes(payload)
            for bad in (False, True):
                responses = list(replies)
                if bad:
                    responses[2] = b'ACK 2047/9999\n'
                port = Port(responses, short_write=True)
                fake_serial = type('SerialModule', (), {'Serial': lambda *a, **kw: port})
                with patch.object(sys, 'argv', ['tool', '--no-reset', '-p', 'FAKE', '-f', str(path)]), \
                        patch.object(flasher, 'serial', fake_serial), \
                        patch.object(flasher.time, 'sleep'), \
                        contextlib.redirect_stdout(io.StringIO()):
                    if bad:
                        with self.assertRaises(SystemExit):
                            flasher.main()
                    else:
                        flasher.main()
                        self.assertTrue(port.written.endswith(payload))
                if bad:
                    self.assertTrue(port.written.endswith(payload[:2048]))


class MetadataTests(unittest.TestCase):
    base = {'id': '3fbb1fe8-2dc4-40d6-955d-8a16d626ece3', 'name': '测试 App', 'target': 'cardenza'}

    def load(self, value, bom=False):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'app.json'
            path.write_text(json.dumps(value, ensure_ascii=False), encoding='utf-8-sig' if bom else 'utf-8')
            return register.load_metadata(path)

    def test_valid_unicode_and_bom(self):
        self.assertEqual(json.loads(self.load(self.base, bom=True)), self.base)
        self.load(dict(self.base, name='测' * 21, build=0xFFFFFFFF))

    def test_rejections(self):
        for change in ({'id': 'INVALID'}, {'name': ' '}, {'name': '测' * 22},
                       {'name': 'A\nB'}, {'description': 'x' * 161}, {'version': 1},
                       {'build': True}, {'build': -1}, {'build': 0x100000000},
                       {'target': 'controller'}, {'source': 'typo'}, {'naem': 'typo'}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.load(dict(self.base, **change))
        with self.assertRaises(ValueError):
            self.load([])


if __name__ == '__main__':
    unittest.main()
