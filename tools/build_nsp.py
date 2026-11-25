#!/usr/bin/env python3
import argparse
import hashlib
import io
import os
import shutil
import struct
import sys
import time
import tomllib
from pathlib import Path

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import padding, rsa
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
TITLE_ID = 0x01004E4945520000
MARKER_NAME = "nier-nx-package.txt"
TOML_NAME = "nier.toml"
DLC_DIR = "0000000000000000"
ICON_SIZE = 256
ICON_CROP = (72, 0, 412, 320)
CNMT_APPLICATION = 0x80

MEDIA_UNIT = 0x200
IVFC_BLOCK_LOG2 = 14
IVFC_BLOCK = 1 << IVFC_BLOCK_LOG2
IVFC_LEVELS = 6
EXEFS_HASH_BLOCK = 0x10000
META_HASH_BLOCK = 0x1000
SDK_VERSION = 0x000C1100


CHUNK = 8 << 20

CONTENT_PROGRAM, CONTENT_META, CONTENT_CONTROL = 0, 1, 2
CNMT_PROGRAM, CNMT_CONTROL = 1, 3


class PackError(Exception):
    pass


def align(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def sha256(data):
    return hashlib.sha256(data).digest()


def load_keys(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError as e:
        raise PackError(f"cannot read the key file {path}: {e.strerror}") from None
    found = {}
    for line in text.splitlines():
        if "=" not in line:
            continue
        name, value = (part.strip() for part in line.split("=", 1))
        found[name.lower()] = value
    keys = {}
    for name, size in (("header_key", 32), ("key_area_key_application_00", 16)):
        value = found.get(name, "")
        try:
            raw = bytes.fromhex(value)
        except ValueError:
            raw = b""
        if len(raw) != size:
            raise PackError(f"{path}: {name} is missing or malformed (expected {size * 2} hex digits)")
        keys[name] = raw
    if keys["header_key"][:16] == keys["header_key"][16:]:
        raise PackError(f"{path}: header_key is malformed (both halves are equal)")
    return keys


class Nro:
    def __init__(self, data):
        if len(data) < 0x80 or data[0x10:0x14] != b"NRO0":
            raise PackError("not an NRO (no NRO0 header)")
        self.data = data
        self.size = struct.unpack_from("<I", data, 0x18)[0]
        self.segments = [struct.unpack_from("<II", data, 0x20 + 8 * i) for i in range(3)]
        self.bss = struct.unpack_from("<I", data, 0x38)[0]
        self.build_id = bytes(data[0x40:0x60])
        for off, size in self.segments:
            if off % 0x1000 or off + size > len(data):
                raise PackError("NRO segments are outside the file or not page aligned")
        if self.segments[0][0] != 0:
            raise PackError("NRO text segment does not start at 0")
        self.assets = {}
        if self.size + 0x38 <= len(data) and data[self.size:self.size + 4] == b"ASET":
            for name, pos in (("icon", 8), ("nacp", 24), ("romfs", 40)):
                off, size = struct.unpack_from("<QQ", data, self.size + pos)
                if size:
                    start = self.size + off
                    if start + size > len(data):
                        raise PackError("NRO asset section is truncated")
                    self.assets[name] = bytes(data[start:start + size])


def nso_from_nro(nro):
    segs = []
    for i, (off, size) in enumerate(nro.segments):
        seg = bytearray(nro.data[off:off + size])
        if i == 0:
            if seg[0x10:0x14] != b"NRO0":
                raise PackError("unexpected NRO layout (header not at text+0x10)")
            seg[0x10:0x80] = bytes(0x70)
        segs.append(bytes(seg))
    header = bytearray(0x100)
    header[0:4] = b"NSO0"
    struct.pack_into("<I", header, 0x0C, 0x38)
    file_off = 0x100
    for i, ((mem_off, _), seg) in enumerate(zip(nro.segments, segs)):
        extra = nro.bss if i == 2 else 1
        struct.pack_into("<IIII", header, 0x10 + 0x10 * i, file_off, mem_off, len(seg), extra)
        struct.pack_into("<I", header, 0x60 + 4 * i, len(seg))
        header[0xA0 + 0x20 * i:0xC0 + 0x20 * i] = sha256(seg)
        file_off += len(seg)
    header[0x40:0x60] = nro.build_id
    return bytes(header) + b"".join(segs)


NACP_ID_FIELDS = (0x3038, 0x3078) + tuple(range(0x30B0, 0x30F0, 8)) + (0x30F8,)
NACP_AOC_BASE = 0x3070


def patch_nacp(nacp, name=None, display_version=None, title_id=None):
    if len(nacp) != 0x4000:
        raise PackError("the NRO's NACP is not 0x4000 bytes")
    nacp = bytearray(nacp)
    if title_id is not None:
        for off in NACP_ID_FIELDS:
            if struct.unpack_from("<Q", nacp, off)[0]:
                struct.pack_into("<Q", nacp, off, title_id)
        if struct.unpack_from("<Q", nacp, NACP_AOC_BASE)[0]:
            struct.pack_into("<Q", nacp, NACP_AOC_BASE, title_id + 0x1000)
    nacp[0x3025] = 0
    nacp[0x3034] = 0
    nacp[0x3035] = 2
    nacp[0x3080:0x3090] = bytes(16)
    if display_version:
        raw = display_version.encode("ascii")[:0xF]
        nacp[0x3060:0x3070] = raw + bytes(0x10 - len(raw))
    if name:
        raw = name.encode("utf-8")[:0x1FF]
        for lang in range(16):
            entry = lang * 0x300
            nacp[entry:entry + 0x200] = raw + bytes(0x200 - len(raw))
    return bytes(nacp)


def npdm_config(title_id):
    return {
        "name": "NieR",
        "program_id": title_id,
        "main_thread_stack_size": 0x100000,
        "main_thread_priority": 44,
        "default_cpu_id": 0,
        "version": 0,
        "address_space_type": 3,
        "is_64_bit": True,
        "is_retail": True,
        "pool_partition": 0,
        "fs_permissions": 0xFFFFFFFFFFFFFFFF,
        "content_owner_ids": [0x0100000000001000],
        "service_host": ["*"],
        "service_access": ["*"],
        "kernel_flags": (59, 28, 2, 0),
        "syscalls": list(range(0xC0)),
        "application_type": 1,
        "min_kernel_version": 0x30,
        "handle_table_size": 512,
        "debug_flags": (False, False, True),
        "map_regions": [(1, True)],
    }


def build_npdm(cfg, acid_modulus=bytes(0x100)):
    caps = []
    hi_prio, lo_prio, hi_cpu, lo_cpu = cfg["kernel_flags"]
    real_hi, real_lo = min(hi_prio, lo_prio), max(hi_prio, lo_prio)
    desc = (((hi_cpu << 8 | lo_cpu) << 6 | (real_hi & 0x3F)) << 6) | (real_lo & 0x3F)
    caps.append((desc << 4) | 0x7)
    descriptors = [0] * 8
    for sc in cfg["syscalls"]:
        descriptors[sc // 0x18] |= 1 << (sc % 0x18)
    for i, d in enumerate(descriptors):
        if d:
            caps.append(((d | (i << 24)) << 5) | 0xF)
    caps.append(((cfg["application_type"] & 7) << 14) | 0x1FFF)
    caps.append(((cfg["min_kernel_version"] & 0xFFFF) << 15) | 0x3FFF)
    caps.append((cfg["handle_table_size"] << 16) | 0x7FFF)
    allow, force_prod, force = cfg["debug_flags"]
    caps.append(((int(allow) | int(force_prod) << 1 | int(force) << 2) << 17) | 0xFFFF)
    regions = cfg["map_regions"] + [(0, False)] * (3 - len(cfg["map_regions"]))
    cap = 0x3FF
    for i, (rtype, ro) in enumerate(regions[:3]):
        cap |= ((rtype & 0x3F) | (int(ro) << 6)) << (11 + 7 * i)
    caps.append(cap)
    kac = struct.pack(f"<{len(caps)}I", *(c & 0xFFFFFFFF for c in caps))

    sac = b""
    for name in cfg["service_host"]:
        sac += bytes([(len(name) - 1) | 0x80]) + name.encode()
    for name in cfg["service_access"]:
        sac += bytes([len(name) - 1]) + name.encode()

    cois = cfg["content_owner_ids"]
    coi = struct.pack("<I", len(cois)) + b"".join(struct.pack("<Q", c) for c in cois) if cois else b""
    fah = struct.pack("<IQIIII", 1, cfg["fs_permissions"], 0x1C, len(coi), 0x1C + len(coi), 0) + coi
    aci0_sac_off = align(0x40 + len(fah), 0x10)
    aci0_kac_off = align(aci0_sac_off + len(sac), 0x10)
    aci0 = bytearray(aci0_kac_off + len(kac))
    struct.pack_into("<4s12xQ8xIIIIII8x", aci0, 0, b"ACI0", cfg["program_id"], 0x40, len(fah),
                     aci0_sac_off, len(sac), aci0_kac_off, len(kac))
    aci0[0x40:0x40 + len(fah)] = fah
    aci0[aci0_sac_off:aci0_sac_off + len(sac)] = sac
    aci0[aci0_kac_off:] = kac

    fac = struct.pack("<BBBxQQQQQ", 1, 0, 0, cfg["fs_permissions"], 0, 0, 0, 0)
    acid_sac_off = align(0x240 + len(fac), 0x10)
    acid_kac_off = align(acid_sac_off + len(sac), 0x10)
    acid = bytearray(acid_kac_off + len(kac))
    acid[0x100:0x200] = acid_modulus
    flags = int(cfg["is_retail"]) | (cfg["pool_partition"] & 3) << 2
    struct.pack_into("<4sIIIQQIIIIII8x", acid, 0x200, b"ACID", len(acid) - 0x100, 0, flags,
                     cfg["program_id"], cfg["program_id"], 0x240, len(fac), acid_sac_off, len(sac),
                     acid_kac_off, len(kac))
    acid[0x240:0x240 + len(fac)] = fac
    acid[acid_sac_off:acid_sac_off + len(sac)] = sac
    acid[acid_kac_off:] = kac

    header = bytearray(0x80)
    mmu = ((cfg["address_space_type"] & 3) << 1) | int(cfg["is_64_bit"])
    name = cfg["name"].encode()[:0xF]
    struct.pack_into("<4sII BBBB II II 16s 16s 48x IIII", header, 0, b"META", 0, 0, mmu, 0,
                     cfg["main_thread_priority"], cfg["default_cpu_id"], 0, 0, cfg["version"],
                     cfg["main_thread_stack_size"], name, b"", 0, 0, 0, 0)
    acid_off = 0x80
    aci0_off = align(acid_off + len(acid), 0x10)
    struct.pack_into("<IIII", header, 0x70, aci0_off, len(aci0), acid_off, len(acid))
    out = bytearray(aci0_off + len(aci0))
    out[:0x80] = header
    out[acid_off:acid_off + len(acid)] = acid
    out[aci0_off:] = aci0
    return bytes(out)


def pfs0_header(entries, header_align=0x20):
    names = b""
    offsets = []
    for name, _ in entries:
        offsets.append(len(names))
        names += name.encode() + b"\0"
    raw = 0x10 + 0x18 * len(entries) + len(names)
    names += bytes(align(raw, header_align) - raw)
    out = bytearray(struct.pack("<4sIII", b"PFS0", len(entries), len(names), 0))
    data_off = 0
    for (name, size), str_off in zip(entries, offsets):
        out += struct.pack("<QQII", data_off, size, str_off, 0)
        data_off += size
    return bytes(out) + names


def pfs0_bytes(files):
    return pfs0_header([(n, len(d)) for n, d in files]) + b"".join(d for _, d in files)


ROMFS_EMPTY = 0xFFFFFFFF
ROMFS_DATA_OFFSET = 0x200


def romfs_hash(parent, name):
    h = parent ^ 123456789
    for c in name:
        h = ((h >> 5) | (h << 27)) & 0xFFFFFFFF
        h ^= c
    return h


def romfs_table_count(n):
    if n < 3:
        return 3
    if n < 19:
        return n | 1
    while any(n % p == 0 for p in (2, 3, 5, 7, 11, 13, 17)):
        n += 1
    return n


class RomfsSource:

    def __init__(self, size, path=None, data=None):
        self.size, self.path, self.data = size, path, data

    def chunks(self):
        if self.data is not None:
            yield self.data
            return
        left = self.size
        with open(self.path, "rb") as fh:
            while left:
                block = fh.read(min(CHUNK, left))
                if not block:
                    raise PackError(f"{self.path} became shorter while packing")
                left -= len(block)
                yield block


class _Dir:
    def __init__(self, name, parent):
        self.name, self.parent = name, parent
        self.dirs, self.files = {}, {}
        self.offset = 0


class Romfs:
    def __init__(self):
        self.root = _Dir(b"", None)
        self.count = 0
        self.data_bytes = 0

    def add(self, path, source):
        parts = [p.encode("utf-8") for p in path.split("/") if p]
        if not parts:
            raise PackError(f"bad RomFS path {path!r}")
        node = self.root
        for part in parts[:-1]:
            if part in node.files:
                raise PackError(f"RomFS path conflict at {path!r}")
            node = node.dirs.setdefault(part, _Dir(part, node))
        if parts[-1] in node.files or parts[-1] in node.dirs:
            raise PackError(f"duplicate RomFS path {path!r}")
        node.files[parts[-1]] = source
        self.count += 1
        self.data_bytes += source.size

    def layout(self):
        dirs = []

        def visit(d):
            dirs.append(d)
            for name in sorted(d.dirs):
                visit(d.dirs[name])

        visit(self.root)
        off = 0
        for d in dirs:
            d.offset = off
            off += 0x18 + align(len(d.name), 4)
        dir_table_size = off
        files = []
        entry_off = 0
        data_off = 0
        for d in dirs:
            for name in sorted(d.files):
                src = d.files[name]
                data_off = align(data_off, 0x10)
                files.append([d, name, src, entry_off, data_off])
                entry_off += 0x20 + align(len(name), 4)
                data_off += src.size
        file_table_size = entry_off
        data_size = data_off

        dir_count, file_count = romfs_table_count(len(dirs)), romfs_table_count(len(files))
        dir_hash = [ROMFS_EMPTY] * dir_count
        file_hash = [ROMFS_EMPTY] * file_count
        dir_table = bytearray(dir_table_size)
        file_table = bytearray(file_table_size)

        file_entry_of = {}
        first_file = {}
        for i, (d, name, src, eoff, doff) in enumerate(files):
            file_entry_of[(id(d), name)] = eoff
            first_file.setdefault(id(d), eoff)
        for d in dirs:
            names = sorted(d.files)
            for i, name in enumerate(names):
                eoff = file_entry_of[(id(d), name)]
                sibling = file_entry_of[(id(d), names[i + 1])] if i + 1 < len(names) else ROMFS_EMPTY
                src = d.files[name]
                h = romfs_hash(d.offset, name) % file_count
                struct.pack_into("<IIQQII", file_table, eoff, d.offset, sibling, 0, src.size, file_hash[h], len(name))
                file_hash[h] = eoff
                file_table[eoff + 0x20:eoff + 0x20 + len(name)] = name
        for d, name, src, eoff, doff in files:
            struct.pack_into("<Q", file_table, eoff + 8, doff)
        for d in dirs:
            children = sorted(d.dirs)
            child = d.dirs[children[0]].offset if children else ROMFS_EMPTY
            if d.parent is None:
                parent, sibling = 0, ROMFS_EMPTY
            else:
                parent = d.parent.offset
                peers = sorted(d.parent.dirs)
                i = peers.index(d.name)
                sibling = d.parent.dirs[peers[i + 1]].offset if i + 1 < len(peers) else ROMFS_EMPTY
            h = romfs_hash(parent if d.parent is not None else 0, d.name) % dir_count
            struct.pack_into("<IIIIII", dir_table, d.offset, parent, sibling, child,
                             first_file.get(id(d), ROMFS_EMPTY), dir_hash[h], len(d.name))
            dir_hash[h] = d.offset
            dir_table[d.offset + 0x18:d.offset + 0x18 + len(d.name)] = d.name

        meta_off = align(ROMFS_DATA_OFFSET + data_size, 0x10)
        dh = struct.pack(f"<{dir_count}I", *dir_hash)
        fh = struct.pack(f"<{file_count}I", *file_hash)
        header = struct.pack("<10Q", 0x50, meta_off, len(dh), meta_off + len(dh), len(dir_table),
                             meta_off + len(dh) + len(dir_table), len(fh),
                             meta_off + len(dh) + len(dir_table) + len(fh), len(file_table), ROMFS_DATA_OFFSET)
        meta = dh + bytes(dir_table) + fh + bytes(file_table)
        self.header = header
        self.meta = meta
        self.meta_off = meta_off
        self.placements = [(doff, src) for _, _, src, _, doff in files]
        self.size = meta_off + len(meta)
        return self.size

    def chunks(self):
        yield self.header + bytes(ROMFS_DATA_OFFSET - len(self.header))
        pos = ROMFS_DATA_OFFSET
        for doff, src in self.placements:
            target = ROMFS_DATA_OFFSET + doff
            if target > pos:
                yield bytes(target - pos)
                pos = target
            for block in src.chunks():
                yield block
                pos += len(block)
        if self.meta_off > pos:
            yield bytes(self.meta_off - pos)
        yield self.meta


class BlockHasher:

    def __init__(self, block, pad_last):
        self.block, self.pad_last = block, pad_last
        self.pending = bytearray()
        self.hashes = []
        self.total = 0

    def update(self, data):
        self.total += len(data)
        mv = memoryview(data)
        if self.pending:
            need = self.block - len(self.pending)
            self.pending += mv[:need]
            mv = mv[need:]
            if len(self.pending) < self.block:
                return
            self.hashes.append(sha256(self.pending))
            self.pending = bytearray()
        full = len(mv) - len(mv) % self.block
        for off in range(0, full, self.block):
            self.hashes.append(hashlib.sha256(mv[off:off + self.block]).digest())
        if full < len(mv):
            self.pending += mv[full:]

    def finish(self):
        if self.pending:
            tail = bytes(self.pending) + (bytes(self.block - len(self.pending)) if self.pad_last else b"")
            self.hashes.append(sha256(tail))
            self.pending = bytearray()
        return b"".join(self.hashes)


def hash_blocks(data, block, pad_last):
    h = BlockHasher(block, pad_last)
    h.update(data)
    return h.finish()


class Section:

    def __init__(self, fs_header, size, chunk_fn, generation=0, secure_value=0):
        assert len(fs_header) == 0x200 and size % MEDIA_UNIT == 0
        fs_header = bytearray(fs_header)
        struct.pack_into("<II", fs_header, 0x140, generation, secure_value)
        self.fs_header, self.size, self.chunks = bytes(fs_header), size, chunk_fn
        self.ctr_upper = (secure_value << 32) | generation


def pfs0_section(pfs0, hash_block):
    table = hash_blocks(pfs0, hash_block, pad_last=False)
    pfs0_off = align(len(table), MEDIA_UNIT)
    body = table + bytes(pfs0_off - len(table)) + pfs0
    body += bytes(align(len(body), MEDIA_UNIT) - len(body))
    fs = bytearray(0x200)
    struct.pack_into("<HBBB", fs, 0, 2, 1, 2, 3)
    fs[0x08:0x28] = sha256(table)
    struct.pack_into("<IIQQQQ", fs, 0x28, hash_block, 2, 0, len(table), pfs0_off, len(pfs0))
    return Section(bytes(fs), len(body), lambda: iter((body,)))


class IvfcPlan:

    def __init__(self, data_hashes, data_size):
        levels = [None] * IVFC_LEVELS
        levels[-1] = (data_size, None)
        below = data_hashes
        for i in range(IVFC_LEVELS - 2, -1, -1):
            levels[i] = (len(below), below)
            below = hash_blocks(below, IVFC_BLOCK, pad_last=True) if i else None
        level1 = levels[0][1]
        if len(level1) > IVFC_BLOCK:
            raise PackError("RomFS too large for 6 IVFC levels")
        self.master = sha256(level1 + bytes(IVFC_BLOCK - len(level1)))
        self.levels = levels
        self.offsets = []
        off = 0
        for size, _ in levels:
            self.offsets.append(off)
            off = align(off + size, IVFC_BLOCK)
        self.size = off


    def fs_header(self):
        fs = bytearray(0x200)
        struct.pack_into("<HBBB", fs, 0, 2, 0, 3, 3)
        struct.pack_into("<4sIII", fs, 0x08, b"IVFC", 0x20000, 0x20, IVFC_LEVELS + 1)
        for i, (size, _) in enumerate(self.levels):
            struct.pack_into("<QQII", fs, 0x18 + 0x18 * i, self.offsets[i], size, IVFC_BLOCK_LOG2, 0)
        fs[0xC8:0xE8] = self.master
        return bytes(fs)

    def chunks(self, data_chunks):
        pos = 0
        for i, (size, blob) in enumerate(self.levels):
            if self.offsets[i] > pos:
                yield bytes(self.offsets[i] - pos)
                pos = self.offsets[i]
            if blob is not None:
                yield blob
                pos += len(blob)
            else:
                for block in data_chunks():
                    yield block
                    pos += len(block)
        if self.size > pos:
            yield bytes(self.size - pos)


class NcaBuilder:
    def __init__(self, keys, title_id, content_type, sections, key_area_key=None, sign_key=None):
        self.keys, self.title_id, self.content_type = keys, title_id, content_type
        self.sections = sections
        self.aes_key = key_area_key or os.urandom(16)
        self.sign_key = sign_key
        self.starts = []
        off = 0xC00
        for s in sections:
            self.starts.append(off)
            off += s.size
        self.size = off

    def header(self):
        h = bytearray(0xC00)
        h[0x200:0x204] = b"NCA3"
        h[0x204] = 0
        h[0x205] = self.content_type
        h[0x206] = 0
        h[0x207] = 0
        struct.pack_into("<QQII", h, 0x208, self.size, self.title_id, 0, SDK_VERSION)
        for i, (s, start) in enumerate(zip(self.sections, self.starts)):
            struct.pack_into("<IIII", h, 0x240 + 0x10 * i, start // MEDIA_UNIT, (start + s.size) // MEDIA_UNIT, 1, 0)
            h[0x280 + 0x20 * i:0x2A0 + 0x20 * i] = sha256(s.fs_header)
            h[0x400 + 0x200 * i:0x600 + 0x200 * i] = s.fs_header
        key_area = bytes(0x20) + self.aes_key + bytes(0x10)
        ecb = Cipher(algorithms.AES(self.keys["key_area_key_application_00"]), modes.ECB()).encryptor()
        h[0x300:0x340] = ecb.update(key_area) + ecb.finalize()
        if self.sign_key is not None:
            h[0x100:0x200] = self.sign_key.sign(bytes(h[0x200:0x400]),
                                                padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32),
                                                hashes.SHA256())
        return encrypt_header(bytes(h), self.keys["header_key"])

    def chunks(self):
        yield self.header()
        for s, start in zip(self.sections, self.starts):
            iv = struct.pack(">QQ", s.ctr_upper, start >> 4)
            enc = Cipher(algorithms.AES(self.aes_key), modes.CTR(iv)).encryptor()
            written = 0
            for block in s.chunks():
                written += len(block)
                yield enc.update(block)
            tail = enc.finalize()
            if tail:
                yield tail
            if written != s.size:
                raise PackError(f"NCA section size mismatch ({written} != {s.size})")

    def to_bytes(self):
        return b"".join(self.chunks())


def encrypt_header(plain, header_key):
    out = bytearray()
    for sector in range(len(plain) // 0x200):
        enc = Cipher(algorithms.AES(header_key), modes.XTS(sector.to_bytes(16, "big"))).encryptor()
        out += enc.update(plain[sector * 0x200:(sector + 1) * 0x200]) + enc.finalize()
    return bytes(out)


def build_cnmt(title_id, version, records):
    ext = struct.pack("<QII", title_id + 0x800, 0, 0)
    out = struct.pack("<QIBBHHHB3xI4x", title_id, version, CNMT_APPLICATION, 0, len(ext), len(records), 0, 0, 0)
    out += ext
    for digest, size, ctype in records:
        out += digest + digest[:16] + size.to_bytes(6, "little") + bytes([ctype, 0])
    return out + bytes(0x20)


def skip_name(name):
    return name.startswith("._") or name in (".DS_Store", "Thumbs.db", "desktop.ini")


def collect_tree(base, prefix, romfs):
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames[:] = sorted(d for d in dirnames if not skip_name(d))
        rel = os.path.relpath(dirpath, base)
        for name in sorted(filenames):
            if skip_name(name):
                continue
            host = os.path.join(dirpath, name)
            path = prefix + ("" if rel == "." else rel.replace(os.sep, "/") + "/") + name
            romfs.add(path, RomfsSource(os.path.getsize(host), path=host))



def stfs_file(package, wanted):
    first = (struct.unpack_from(">I", package, 0x340)[0] + 0xFFF) & ~0xFFF
    shift = 0 if (first >> 12) & 0xF == 0xB else 1

    def offset(block):
        backing = ((block + 0xAA) // 0xAA) << shift
        if block >= 0xAA:
            backing += ((block + 0x70E4) // 0x70E4) << shift
        if block >= 0x70E4:
            backing += 1 << shift
        return first + ((backing + block) << 12)

    table = offset(int.from_bytes(package[0x37E:0x381], "little"))
    for entry in range(table, table + 0x1000, 0x40):
        length = package[entry + 0x28] & 0x3F
        if not length:
            break
        if package[entry:entry + length].decode("ascii") != wanted:
            continue
        start = int.from_bytes(package[entry + 0x2F:entry + 0x32], "little")
        size = struct.unpack_from(">I", package, entry + 0x34)[0]
        blocks = (size + 0xFFF) >> 12
        return b"".join(package[offset(start + i):offset(start + i) + 0x1000] for i in range(blocks))[:size]
    raise PackError(f"{wanted} not found in the disc art package")


def disc_icon(game_root):
    art = Image.open(io.BytesIO(stfs_file((game_root / "nxeart").read_bytes(), "nxeslot.jpg"))).convert("RGB")
    return art.crop(ICON_CROP)


def icon_bytes(image):
    out = io.BytesIO()
    image.convert("RGB").resize((ICON_SIZE, ICON_SIZE), Image.LANCZOS).save(out, "JPEG", quality=90)
    return out.getvalue()


def build_romfs(game_root, toml, marketplace, language, marker):
    romfs = Romfs()
    with open(toml, "rb") as fh:
        tomllib.load(fh)
    romfs.add(TOML_NAME, RomfsSource(os.path.getsize(toml), path=toml))
    if not (game_root / "default.xex").is_file():
        raise PackError(f"{game_root}/default.xex not found")
    collect_tree(game_root, "game_root/", romfs)
    if marketplace and (marketplace / DLC_DIR).is_dir():
        collect_tree(marketplace / DLC_DIR, f"marketplace/{DLC_DIR}/", romfs)
    if language:
        if not (language / "files.txt").is_file():
            raise PackError(f"{language}/files.txt not found, build the language pack first")
        collect_tree(language, "lang/", romfs)
    romfs.add(MARKER_NAME, RomfsSource(len(marker), data=marker))
    return romfs


def romfs_section(romfs, progress=None):
    romfs.layout()
    hasher = BlockHasher(IVFC_BLOCK, pad_last=True)
    done = 0
    for block in romfs.chunks():
        hasher.update(block)
        done += len(block)
        if progress:
            progress(done, romfs.size)
    if hasher.total != romfs.size:
        raise PackError("RomFS size changed while hashing")
    plan = IvfcPlan(hasher.finish(), romfs.size)
    return Section(plan.fs_header(), plan.size, lambda: plan.chunks(romfs.chunks))


def human(n):
    for unit in ("B", "KiB", "MiB", "GiB"):
        if n < 1024 or unit == "GiB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{n} B"
        n /= 1024


def progress(label, log):
    clock = {"start": time.time(), "last": 0.0}

    def report(done, total):
        now = time.time()
        if now - clock["last"] >= 5 or done == total:
            clock["last"] = now
            rate = done / max(now - clock["start"], 1e-6)
            log(f"  {label}: {done * 100 // max(total, 1)}% ({human(done)} of {human(total)}, {human(rate)}/s)")

    return report


def write_nsp(output, program, control, make_meta, log):
    meta_size = len(make_meta(bytes(32), bytes(32)))
    names = [("0" * 32 + ".nca", program.size), ("0" * 32 + ".nca", len(control)), ("0" * 32 + ".cnmt.nca", meta_size)]
    header_size = len(pfs0_header(names))
    total = header_size + sum(size for _, size in names)
    output.parent.mkdir(parents=True, exist_ok=True)
    free = shutil.disk_usage(output.parent).free
    if free < total + (64 << 20):
        raise PackError(f"not enough free space in {output.parent}: the NSP needs {human(total)}, {human(free)} free")
    log(f"NSP: {human(total)} -> {output}")
    done = False
    try:
        with open(output, "w+b") as out:
            out.seek(header_size)
            digest = hashlib.sha256()
            written = 0
            report = progress("write", log)
            for block in program.chunks():
                out.write(block)
                digest.update(block)
                written += len(block)
                report(written, program.size)
            if written != program.size:
                raise PackError("Program NCA size mismatch")
            program_hash = digest.digest()
            control_hash = sha256(control)
            out.write(control)
            meta = make_meta(program_hash, control_hash)
            if len(meta) != meta_size:
                raise PackError("Meta NCA size changed")
            out.write(meta)
            final = [(program_hash[:16].hex() + ".nca", program.size), (control_hash[:16].hex() + ".nca", len(control)),
                     (sha256(meta)[:16].hex() + ".cnmt.nca", len(meta))]
            out.seek(0)
            out.write(pfs0_header(final))
        done = True
    finally:
        if not done:
            output.unlink(missing_ok=True)
    return total


def build(args, log=print):
    keys = load_keys(args.keys)
    nro = Nro(Path(args.nro).read_bytes())
    if "nacp" not in nro.assets:
        raise PackError("the NRO has no NACP, build it with tools/build_nro.sh")
    game_root = Path(args.game_root)
    icon = icon_bytes(Image.open(args.icon) if args.icon else disc_icon(game_root))

    sign_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    modulus = sign_key.public_key().public_numbers().n.to_bytes(0x100, "big")
    npdm = build_npdm(npdm_config(args.title_id), modulus)
    exefs = pfs0_section(pfs0_bytes([("main", nso_from_nro(nro)), ("main.npdm", npdm)]), EXEFS_HASH_BLOCK)

    marker = f"title_id={args.title_id:016x}\nnro_sha256={hashlib.sha256(nro.data).hexdigest()}\n".encode()
    romfs = build_romfs(game_root, args.toml, None if args.no_dlc else Path(args.marketplace),
                        Path(args.language) if args.language else None, marker)
    log(f"RomFS: {romfs.count} files, {human(romfs.data_bytes)}")
    log("Pass 1/2: hashing the game data")
    program = NcaBuilder(keys, args.title_id, CONTENT_PROGRAM, [exefs, romfs_section(romfs, progress("hash", log))],
                         sign_key=sign_key)

    control_romfs = Romfs()
    nacp = patch_nacp(nro.assets["nacp"], args.name, None, args.title_id)
    control_romfs.add("control.nacp", RomfsSource(len(nacp), data=nacp))
    control_romfs.add("icon_AmericanEnglish.dat", RomfsSource(len(icon), data=icon))
    control = NcaBuilder(keys, args.title_id, CONTENT_CONTROL, [romfs_section(control_romfs)]).to_bytes()

    def make_meta(program_hash, control_hash):
        cnmt = build_cnmt(args.title_id, 0, [(program_hash, program.size, CNMT_PROGRAM),
                                             (control_hash, len(control), CNMT_CONTROL)])
        section = pfs0_section(pfs0_bytes([(f"Application_{args.title_id:016x}.cnmt", cnmt)]), META_HASH_BLOCK)
        return NcaBuilder(keys, args.title_id, CONTENT_META, [section]).to_bytes()

    log("Pass 2/2: encrypting and writing")
    return write_nsp(Path(args.output), program, control, make_meta, log)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Packs the port and your own game data into one installable NSP")
    parser.add_argument("--keys", default=os.environ.get("SWITCH_PROD_KEYS", os.path.expanduser("~/.switch/prod.keys")))
    parser.add_argument("--output", default=str(ROOT / "out" / "nsp" / "nier-nx.nsp"))
    parser.add_argument("--nro", default=str(ROOT / "out" / "nx" / "nier-nx.nro"))
    parser.add_argument("--toml", default=str(ROOT / "out" / "nx" / "nier.toml"))
    parser.add_argument("--game-root", default=str(ROOT / "assets" / "game_root"))
    parser.add_argument("--marketplace", default=str(ROOT / "assets" / "marketplace"))
    parser.add_argument("--no-dlc", action="store_true")
    parser.add_argument("--language")
    parser.add_argument("--icon")
    parser.add_argument("--name")
    parser.add_argument("--title-id", type=lambda text: int(text, 16), default=TITLE_ID)
    args = parser.parse_args(argv)
    if args.title_id & 0xFFF or not 0x0100000000000000 <= args.title_id <= 0x0FFFFFFFFFFFF000:
        parser.error("the title ID must look like 01xxxxxxxxxxx000")
    try:
        build(args)
    except PackError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
