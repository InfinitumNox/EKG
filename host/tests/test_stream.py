import numpy as np

from ekg.device.stream import StreamAssembler
from ekg.protocol import Flag, SamplesPacket
from ekg.ringbuffer import RingBuffer


def pkt(first, n=25, seq=0):
    return SamplesPacket(seq, first, Flag.NONE, np.arange(n, dtype=np.uint32))


def test_contiguous_and_gap():
    a = StreamAssembler()
    c0 = a.push(pkt(1000))
    assert c0.start == 0 and c0.gap_before == 0
    c1 = a.push(pkt(1025))
    assert c1.start == 25
    c2 = a.push(pkt(1100))  # 50 Samples fehlen
    assert c2.start == 100 and c2.gap_before == 50
    assert a.stats.lost_samples == 50 and a.stats.gaps == 1


def test_restart_continues_numbering():
    a = StreamAssembler()
    a.push(pkt(5000))
    c = a.push(pkt(0))
    assert c.restart and c.start == 25 and c.gap_before == 0


def test_32bit_wrap():
    a = StreamAssembler()
    a.push(pkt(2**32 - 25))
    c = a.push(pkt(0))
    assert not c.restart and c.start == 25


def test_ringbuffer_wraps_and_fills():
    rb = RingBuffer(10, np.float32)
    rb.append(np.arange(7))
    rb.append(np.arange(7, 15))
    assert rb.end == 15 and rb.start == 5
    np.testing.assert_array_equal(rb.get(5, 15), np.arange(5, 15))
    got = rb.get(0, 8)
    assert np.isnan(got[:5]).all()
    np.testing.assert_array_equal(got[5:], [5, 6, 7])
    rb.append(np.arange(100))
    np.testing.assert_array_equal(rb.last(3)[1], [97, 98, 99])
