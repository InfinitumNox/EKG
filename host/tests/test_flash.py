import struct

import pytest

from ekg import flash as F


def uf2(path, family, n=4):
    blocks = []
    for i in range(n):
        hdr = struct.pack(
            "<IIIIIIII",
            F.UF2_MAGIC0,
            F.UF2_MAGIC1,
            F.UF2_FLAG_FAMILY,
            0x10000000 + 256 * i,
            256,
            i,
            n,
            family,
        )
        blocks.append(hdr + bytes(476) + struct.pack("<I", 0x0AB16F30))
    path.write_bytes(b"".join(blocks))
    return path


def test_check_uf2(tmp_path):
    assert F.check_uf2(uf2(tmp_path / "a.uf2", 0xE48BFF59)) == 4
    with pytest.raises(F.FlashError, match="RP2040"):
        F.check_uf2(uf2(tmp_path / "b.uf2", F.FAMILY_RP2040))
    (tmp_path / "c.uf2").write_bytes(b"x" * 100)
    with pytest.raises(F.FlashError, match="keine UF2"):
        F.check_uf2(tmp_path / "c.uf2")


def test_prebuilt_firmware_is_valid():
    p = F.default_uf2()
    if p is None:
        pytest.skip("keine Firmware im Repository")
    assert F.check_uf2(p) > 100


def test_find_drive(tmp_path):
    root = tmp_path / "media"
    (root / "USBSTICK").mkdir(parents=True)
    assert F.find_drive([root]) is None
    d = root / "RP2350"
    d.mkdir()
    (d / "INFO_UF2.TXT").write_text(
        "UF2 Bootloader v1.0\nModel: Raspberry Pi RP2350\nBoard-ID: RP2350\n"
    )
    assert F.find_drive([root]) == d
    (d / "INFO_UF2.TXT").write_text("Board-ID: RPI-RP2\nModel: Raspberry Pi RP2040\n")
    with pytest.raises(F.FlashError, match="RP2040"):
        F.find_drive([root])


def test_flash_copies_to_drive(tmp_path, monkeypatch):
    root = tmp_path / "media"
    d = root / "RP2350"
    d.mkdir(parents=True)
    (d / "INFO_UF2.TXT").write_text("Board-ID: RP2350\n")
    fw = uf2(tmp_path / "ekg.uf2", 0xE48BFF59)
    monkeypatch.setattr(F, "mount_roots", lambda: [root])
    monkeypatch.setattr(F, "mount_with_udisks", lambda: None)
    from ekg.device import transport

    monkeypatch.setattr(
        transport, "find_ports", lambda: [transport.PortInfo("/dev/ttyACM0", "Pico EKG")]
    )
    port = F.flash(fw, wait_s=1, log=lambda _m: None)
    assert port == "/dev/ttyACM0"
    assert (d / "ekg.uf2").read_bytes() == fw.read_bytes()


def test_flash_without_device_fails_cleanly(tmp_path, monkeypatch):
    fw = uf2(tmp_path / "ekg.uf2", 0xE48BFF59)
    monkeypatch.setattr(F, "mount_roots", lambda: [tmp_path / "nichts"])
    monkeypatch.setattr(F, "mount_with_udisks", lambda: None)
    monkeypatch.setattr(F, "trigger_bootsel", lambda: False)
    with pytest.raises(F.FlashError, match="Kein Laufwerk"):
        F.flash(fw, wait_s=0.6, log=lambda _m: None)
