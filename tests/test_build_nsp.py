import hashlib
import os
import struct
import sys
from pathlib import Path

import pytest
from PIL import Image

pytest.importorskip("cryptography")

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import build_nsp


def fake_nro() -> bytes:
    segments = [os.urandom(0x1000) for _ in range(3)]
    text = bytearray(segments[0])
    text[0x10:0x80] = bytes(0x70)
    text[0x10:0x14] = b"NRO0"
    struct.pack_into("<I", text, 0x18, 0x3000)
    for index in range(3):
        struct.pack_into("<II", text, 0x20 + 8 * index, 0x1000 * index, 0x1000)
    struct.pack_into("<I", text, 0x38, 0x2000)
    text[0x40:0x60] = os.urandom(32)
    assets = bytearray(0x38)
    assets[0:4] = b"ASET"
    struct.pack_into("<QQ", assets, 24, 0x38, 0x4000)
    return bytes(text) + segments[1] + segments[2] + bytes(assets) + bytes(0x4000)


def pack(tmp_path: Path) -> tuple:
    game_root = tmp_path / "game_root"
    (game_root / "media").mkdir(parents=True)
    (game_root / "default.xex").write_bytes(os.urandom(70000))
    (game_root / "media" / "layer.cpk").write_bytes(os.urandom(200001))
    dlc = tmp_path / "marketplace" / "0000000000000000" / "535107E8"
    dlc.mkdir(parents=True)
    (dlc / "AutoLoad.ini").write_bytes(b"dlc")
    language = tmp_path / "lang"
    language.mkdir()
    (language / "files.txt").write_bytes(b"menu.bin\n")
    (language / "menu.bin").write_bytes(b"text")
    keys = {"header_key": os.urandom(32), "key_area_key_application_00": os.urandom(16)}
    (tmp_path / "test.keys").write_text("".join(f"{name} = {value.hex()}\n" for name, value in keys.items()))
    (tmp_path / "nier-nx.nro").write_bytes(fake_nro())
    (tmp_path / "nier.toml").write_text('log_level = "info"\n')
    Image.new("RGB", (64, 64), (40, 60, 80)).save(tmp_path / "icon.png")
    output = tmp_path / "out" / "nier-nx.nsp"
    code = build_nsp.main([
        "--keys", str(tmp_path / "test.keys"), "--output", str(output), "--nro", str(tmp_path / "nier-nx.nro"),
        "--toml", str(tmp_path / "nier.toml"), "--game-root", str(game_root),
        "--marketplace", str(tmp_path / "marketplace"), "--language", str(language),
        "--icon", str(tmp_path / "icon.png"),
    ])
    assert code == 0
    return output.read_bytes(), keys


def pfs0_files(data: bytes) -> dict:
    assert data[:4] == b"PFS0"
    count, names_size = struct.unpack_from("<II", data, 4)
    names = 0x10 + 0x18 * count
    base = names + names_size
    files = {}
    for index in range(count):
        offset, size, name = struct.unpack_from("<QQI", data, 0x10 + 0x18 * index)
        end = data.index(b"\0", names + name)
        files[data[names + name:end].decode()] = data[base + offset:base + offset + size]
    return files


def nca_header(nca: bytes, key: bytes) -> bytes:
    out = bytearray()
    for sector in range(6):
        cipher = Cipher(algorithms.AES(key), modes.XTS(sector.to_bytes(16, "big"))).decryptor()
        out += cipher.update(nca[sector * 0x200:(sector + 1) * 0x200]) + cipher.finalize()
    return bytes(out)


def section(nca: bytes, header: bytes, keys: dict, index: int) -> bytes:
    start, end = (unit * 0x200 for unit in struct.unpack_from("<II", header, 0x240 + 0x10 * index))
    ecb = Cipher(algorithms.AES(keys["key_area_key_application_00"]), modes.ECB()).decryptor()
    key = (ecb.update(header[0x300:0x340]) + ecb.finalize())[0x20:0x30]
    upper = struct.unpack_from("<Q", header, 0x400 + 0x200 * index + 0x140)[0]
    ctr = Cipher(algorithms.AES(key), modes.CTR(struct.pack(">QQ", upper, start >> 4))).decryptor()
    return ctr.update(nca[start:end]) + ctr.finalize()


def test_nsp_holds_three_ncas_named_by_their_hashes(tmp_path):
    nsp, keys = pack(tmp_path)
    files = pfs0_files(nsp)
    assert len(files) == 3
    for name, nca in files.items():
        assert name.split(".")[0] == hashlib.sha256(nca).hexdigest()[:32]
        header = nca_header(nca, keys["header_key"])
        assert header[0x200:0x204] == b"NCA3"
        assert struct.unpack_from("<Q", header, 0x210)[0] == build_nsp.TITLE_ID


def test_program_carries_the_program_and_the_game_data(tmp_path):
    nsp, keys = pack(tmp_path)
    program = max(pfs0_files(nsp).values(), key=len)
    header = nca_header(program, keys["header_key"])
    exefs = section(program, header, keys, 0)
    table_size, pfs0_offset, pfs0_size = struct.unpack_from("<QQQ", header, 0x400 + 0x38)
    assert hashlib.sha256(exefs[:table_size]).digest() == header[0x408:0x428]
    program_files = pfs0_files(exefs[pfs0_offset:pfs0_offset + pfs0_size])
    assert set(program_files) == {"main", "main.npdm"}
    main = program_files["main"]
    assert main[:4] == b"NSO0"
    for index in range(3):
        offset, _, size = struct.unpack_from("<III", main, 0x10 + 0x10 * index)
        assert hashlib.sha256(main[offset:offset + size]).digest() == main[0xA0 + 0x20 * index:0xC0 + 0x20 * index]
    romfs = section(program, header, keys, 1)
    data_offset = struct.unpack_from("<Q", header, 0x600 + 0x18 + 0x18 * 5)[0]
    image = romfs[data_offset:]
    for path in ("default.xex", "media/layer.cpk"):
        assert (tmp_path / "game_root" / path).read_bytes() in image
    for name in (b"nier-nx-package.txt", b"nier.toml", b"AutoLoad.ini", b"files.txt", b"menu.bin"):
        assert name in image


def test_bad_key_file_is_refused(tmp_path):
    (tmp_path / "bad.keys").write_text("header_key = 00\n")
    assert build_nsp.main(["--keys", str(tmp_path / "bad.keys"), "--output", str(tmp_path / "x.nsp")]) == 1
    assert not (tmp_path / "x.nsp").exists()
