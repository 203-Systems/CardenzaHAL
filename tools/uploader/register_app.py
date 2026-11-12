#!/usr/bin/env python3
"""List Launcher apps or register the verified image in an exact partition.

Return to Launcher's home screen first. This tool does not flash or reset.
Derived from 203-Systems/203-launcher tools/register_app.py (MIT).
"""
import argparse
import json
from pathlib import Path
import re
import sys

from serial_flasher import (
    serial, require_serial, LineReader, wait_for_launcher, send_command,
    require_reply, positive_number,
)


def load_metadata(path):
    metadata = json.loads(path.read_text(encoding='utf-8-sig'))
    if not isinstance(metadata, dict):
        raise ValueError('Metadata must be a JSON object')
    allowed = {'id', 'name', 'description', 'version', 'build', 'target', 'source'}
    unknown = set(metadata) - allowed
    if unknown:
        raise ValueError(f'Unknown metadata fields: {", ".join(sorted(unknown))}')
    app_id = metadata.get('id')
    if not isinstance(app_id, str) or not re.fullmatch(
        r'[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}', app_id
    ):
        raise ValueError('id must be a lowercase, hyphenated UUID')
    for key, limit in [('name', 64), ('description', 160), ('version', 32)]:
        value = metadata.get(key, '')
        if not isinstance(value, str):
            raise ValueError(f'{key} must be a string')
        if len(value.encode('utf-8')) > limit or any(ord(c) < 32 or ord(c) == 127 for c in value):
            raise ValueError(f'{key} must be at most {limit} UTF-8 bytes without control characters')
    if not metadata.get('name', '').strip():
        raise ValueError('name is required and must not be blank')
    build = metadata.get('build', 0)
    if type(build) is not int or not 0 <= build <= 0xFFFFFFFF:
        raise ValueError('build must be an integer from 0 to 4294967295')
    if 'target' in metadata and metadata['target'] != 'cardenza':
        raise ValueError('target must be cardenza')
    if 'source' in metadata and metadata['source'] not in ('manual', 'developer'):
        raise ValueError('source must be manual or developer')
    payload = json.dumps(metadata, ensure_ascii=False, separators=(',', ':'), allow_nan=False)
    if len(payload.encode('utf-8')) > 1024:
        raise ValueError('Metadata exceeds 1024 UTF-8 bytes')
    return payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('-p', '--port', required=True, help='COM8 or /dev/cu.usbmodem...')
    parser.add_argument('--label', help='Exact app partition label from --list')
    parser.add_argument('-b', '--baud', type=int, default=115200)
    parser.add_argument('--boot-timeout', type=positive_number, default=10)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument('--list', action='store_true')
    action.add_argument('--metadata', type=Path, help='App metadata JSON file')
    args = parser.parse_args()
    if args.baud <= 0:
        parser.error('--baud must be positive')
    command = 'apps list'
    if args.metadata:
        if not args.label or not re.fullmatch(r'[A-Za-z0-9_-]{1,15}', args.label):
            parser.error('--label must contain 1–15 ASCII letters, digits, underscores or hyphens')
        command = f'apps register {args.label} {load_metadata(args.metadata)}'
    elif args.label:
        parser.error('--label is only used with --metadata')
    require_serial()
    with serial.Serial(args.port, args.baud, timeout=0.2, write_timeout=10) as port:
        reader = LineReader(port)
        if not wait_for_launcher(port, reader, args.boot_timeout):
            raise ValueError('Launcher did not respond; return to its home screen and retry')
        send_command(port, command)
        response = reader.wait_for_line(
            lambda line: line.startswith(('ERR', 'OK registered', '[')), 60)
        if response is None or response.startswith('ERR'):
            raise ValueError(response or 'Timed out waiting for Launcher')
        if args.list:
            apps = json.loads(response)
            if not isinstance(apps, list):
                raise ValueError('Expected an application list from Launcher')
            print(json.dumps(apps, ensure_ascii=False, indent=2))
        else:
            require_reply(response, 'OK registered')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError) as exc:
        sys.exit(f'Error: {exc}')
    except Exception as exc:
        if serial is not None and isinstance(exc, serial.SerialException):
            sys.exit(f'Serial error: {exc}')
        raise
    except KeyboardInterrupt:
        sys.exit('Cancelled')
