#!/usr/bin/env python3
"""Regression checks for native memory imports and process-parameter edits."""
import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('native_startup', ROOT / 'tools/native-startup.py')
startup = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(startup)


def fixture():
    data = bytearray(0x3000)
    ident = b'\x7fELF\x02\x01\x01' + bytes(9)
    struct.pack_into('<16sHHIQQQIHHHHHH', data, 0, ident, 0xfe10, 62, 1,
                     0x120, 64, 0, 0, 64, 56, 3, 0, 0, 0)
    headers = [(1, 6, 0, 0, 0, len(data), len(data), 0x4000),
               (0x61000001, 4, 0x1000, 0x1000, 0, 0x60, 0x60, 8),
               (2, 6, 0x2000, 0x2000, 0, 64, 64, 8)]
    for i, header in enumerate(headers):
        struct.pack_into('<IIQQQQQQ', data, 64 + 56 * i, *header)
    struct.pack_into('<Q4sI', data, 0x1000, 0x60, b'ORBI', 5)
    offsets = [0, 0xa8, 0xe0, 0xf0, 0x168, 0x228]
    sizes = [0xa8, 0x38, 0x10, 0x78, 0xc0, 0x38]
    counts = [0x10000000e, 0, 0, 2, 3, 1]
    for offset, size, count in zip(offsets, sizes, counts):
        struct.pack_into('<QQ', data, 0x1060 + offset, size, count)
    struct.pack_into('<QI', data, 0x12c0, 0xffffffffffffffff, 1)
    relocs = [(0x1038, 0x1060), (0x1040, 0x1108), (0x1048, 0x1140),
              (0x1090, 0x1150), (0x1098, 0x11c8), (0x10c0, 0x1288),
              (0x1070, 0x12c0), (0x1080, 0x12c8)]
    for i, (offset, target) in enumerate(relocs):
        struct.pack_into('<QQq', data, 0x2100 + 24 * i, offset, 8, target)
    for i, entry in enumerate(((7, 0x2100), (8, 24 * len(relocs)), (9, 24), (0, 0))):
        struct.pack_into('<qQ', data, 0x2000 + 16 * i, *entry)
    return data


class NativeStartupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='stremio-native-startup-')
        self.folder = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_heap_update_is_restricted_and_idempotent(self):
        path = self.folder / 'native.elf'
        before = fixture()
        path.write_bytes(before)
        result = startup.configure_process(path)
        after = path.read_bytes()
        self.assertEqual(result['processHeapBytes'], 256 << 20)
        self.assertEqual(before[:0x12c0], after[:0x12c0])
        self.assertEqual(before[0x12c8:], after[0x12c8:])
        startup.configure_process(path)
        self.assertEqual(path.read_bytes(), after)

    def test_heap_rejects_corrupted_layout_without_writing(self):
        mutations = ((0x1008, b'BAD!'), (0x1060, struct.pack('<Q', 0xff)),
                     (0x2108, struct.pack('<Q', 1)),
                     (0x2110, struct.pack('<Q', 0x123456)),
                     (0x12c0, struct.pack('<Q', 1)))
        for offset, value in mutations:
            with self.subTest(offset=offset):
                data = fixture()
                data[offset:offset + len(value)] = value
                path = self.folder / 'corrupt.elf'
                path.write_bytes(data)
                with self.assertRaises(ValueError):
                    startup.configure_process(path)
                self.assertEqual(path.read_bytes(), data)

    def test_payload_archive_is_filtered_without_modifying_sdk(self):
        members = []
        for name in ('mman.o', 'syscalls.o', 'locale.o'):
            path = self.folder / name
            path.write_bytes(name.encode())
            members.append(str(path))
        source, output = self.folder / 'libc.a', self.folder / 'native.a'
        subprocess.run(['llvm-ar-18', 'rcs', str(source), *members], check=True)
        before = source.read_bytes()
        result = startup.prepare_libc(source, output, 'llvm-ar-18')
        self.assertEqual(result['remainingObjects'], 1)
        self.assertEqual(source.read_bytes(), before)
        with self.assertRaises(ValueError):
            startup.prepare_libc(source, source, 'llvm-ar-18')

    def test_disassembly_gate_distinguishes_instructions_from_data(self):
        for instruction in ('', 'syscall', 'sysenter', 'int $0x80'):
            with self.subTest(instruction=instruction):
                source = self.folder / 'probe.s'
                output = self.folder / 'probe.o'
                source.write_text('.text\n.globl startup_probe\nstartup_probe:\n'
                                  'call mmap\ncall mprotect\n' + instruction + '\nret\n'
                                  '.section .rodata\n.byte 0x0f, 0x05\n')
                subprocess.run(['clang-18', '-c', str(source), '-o', str(output)], check=True)
                if instruction:
                    with self.assertRaisesRegex(ValueError, 'Payload-only system calls'):
                        startup.audit(output, 'llvm-objdump-18', 'llvm-nm-18')
                else:
                    result = startup.audit(output, 'llvm-objdump-18', 'llvm-nm-18')
                    self.assertEqual(result['inlineSystemCallInstructions'], 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
