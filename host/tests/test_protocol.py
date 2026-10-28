import numpy as np
import pytest

from ekg import protocol as P

# Identisch mit firmware/tests/test_firmware.c
GOLDEN_PAYLOAD = "010134120403020103050000000001000000efcdab006d80"
GOLDEN_FRAME = "0b010134120403020103050101010201010104efcdab036d8000"


def test_crc_check_value():
    assert P.crc16(b"123456789") == 0x29B1


@pytest.mark.parametrize(
    ("raw", "enc"),
    [
        ("00", "0101"),
        ("0000", "010101"),
        ("11220033", "0311220233"),
        ("11223344", "0511223344"),
        ("11000000", "0211010101"),
    ],
)
def test_cobs_vectors(raw, enc):
    assert P.cobs_encode(bytes.fromhex(raw)).hex() == enc
    assert P.cobs_decode(bytes.fromhex(enc)).hex() == raw


def test_cobs_long_blocks():
    data = bytes(range(1, 255))
    enc = P.cobs_encode(data)
    assert len(enc) == 255 and enc[0] == 0xFF
    assert P.cobs_decode(enc) == data
    data2 = data + b"\xff"
    enc2 = P.cobs_encode(data2)
    assert len(enc2) == 257
    assert P.cobs_decode(enc2) == data2


def test_cobs_roundtrip_random(rng):
    for n in (0, 1, 253, 254, 255, 508, 1000):
        data = rng.integers(0, 256, n, dtype=np.uint8).tobytes()
        enc = P.cobs_encode(data)
        assert 0 not in enc
        assert P.cobs_decode(enc) == data


def test_golden_samples_frame():
    payload = P.build_samples(0x1234, 0x01020304, 0x05, [0, 1, 0x00ABCDEF])
    assert payload.hex() == GOLDEN_PAYLOAD
    assert P.frame(payload).hex() == GOLDEN_FRAME
    pkt = P.parse_payload(payload)
    assert isinstance(pkt, P.SamplesPacket)
    assert pkt.seq == 0x1234 and pkt.first_index == 0x01020304
    assert pkt.flags == P.Flag.LO_PLUS | P.Flag.OVERRUN
    assert list(pkt.samples) == [0, 1, 0x00ABCDEF]


def test_roundtrip_other_types():
    info = P.InfoPacket(
        3,
        (0, 1, 2),
        500,
        64,
        12,
        3300,
        1100,
        P.Mode.TEST,
        True,
        "0102030405060708",
        "Pico EKG AD8232",
    )
    assert P.parse_payload(P.build_info(info)) == info
    st = P.StatusPacket(4, 123456, 1, 2, 3, P.Mode.LIVE, False, P.CalWave.SINE, P.Flag.LO_MINUS)
    assert P.parse_payload(P.build_status(st)) == st
    ack = P.parse_payload(P.build_ack(5, P.Opcode.START, P.AckStatus.BAD_STATE))
    assert ack == P.AckPacket(5, P.Opcode.START, P.AckStatus.BAD_STATE)
    cmd = P.parse_payload(P.build_command(6, P.Opcode.SET_RATE, P.cmd_args_set_rate(1000)))
    assert cmd == P.CommandPacket(6, P.Opcode.SET_RATE, b"\xe8\x03")


def test_corruption_detected():
    payload = bytearray(P.build_samples(1, 0, 0, [10, 20, 30]))
    payload[8] ^= 0x40
    with pytest.raises(P.ProtocolError, match="CRC"):
        P.parse_payload(bytes(payload))


def test_decoder_arbitrary_chunking(rng):
    frames = [P.frame(P.build_samples(i, i * 25, 0, np.arange(25) + i)) for i in range(50)]
    stream = b"".join(frames)
    dec = P.FrameDecoder()
    out = []
    pos = 0
    while pos < len(stream):
        n = int(rng.integers(1, 64))
        out += dec.feed(stream[pos : pos + n])
        pos += n
    assert [p.seq for p in out] == list(range(50))
    assert dec.errors == 0


def test_decoder_survives_garbage(rng):
    """Zufällige Bytes dürfen nie eine Exception auslösen; gültige Frames danach kommen an."""
    dec = P.FrameDecoder()
    for _ in range(200):
        dec.feed(rng.integers(0, 256, int(rng.integers(1, 600)), dtype=np.uint8).tobytes())
    good = P.frame(P.build_samples(7, 0, 0, [1, 2, 3]))
    out = dec.feed(b"\x00" + good)
    assert any(isinstance(p, P.SamplesPacket) and p.seq == 7 for p in out)
    assert dec.errors > 0


def test_decoder_rejects_oversized_frames():
    dec = P.FrameDecoder()
    dec.feed(b"\x01" * 2000)
    out = dec.feed(b"\x00" + P.frame(P.build_ack(1, 1, 0)))
    assert len(out) == 1
    assert "zu lang" in dec.last_error
