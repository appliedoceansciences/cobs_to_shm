#!/usr/bin/env python3
import sys
import struct
import time

from shared_memory_ringbuffer_reader import shared_memory_ringbuffer_reader_recv_blocking, shared_memory_ringbuffer_reader_init
from parse_acoustic_packets import parse_acoustic_packet

def next_acoustic_packet(shm):
    # loop until we can return an acoustic packet
    while True:
        # this returns None if the eof condition has been reached
        payload = shared_memory_ringbuffer_reader_recv_blocking(shm)
        if not payload: return None

        # interpret these eight bytes as a 16-bit packet size and 48-bit timestamp
        _, timestamp_lsbs, timestamp_msbs = struct.unpack('<HHI', payload[0:8])
        logged_timestamp_microseconds = ((timestamp_msbs << 16) | timestamp_lsbs) * 16

        # make sure the payload bytes are an acoustic packet and not something else
        packet = parse_acoustic_packet(payload[8:], logged_timestamp_microseconds)
        if packet: return packet

def main():
    shm_name = sys.argv[1] if len(sys.argv) > 1 else '/cobs_to_shm'

    # loop until shm exists and is being written to
    while True:
        shm = shared_memory_ringbuffer_reader_init(shm_name)
        if shm is not None: break
        time.sleep(0.05)

    # get the first packet
    packet = next_acoustic_packet(shm)

    approx_us_per_packet = packet.samples.shape[0] * 1e6 / packet.fs

    while True:
        packet_prev = packet
        time.sleep(5)

        # fast forward over all packets that have been received
        shm.reader_cursor = shm.view_of_writer_cursor[0]
        packet = next_acoustic_packet(shm)
        if packet is None: break

        # note that seqnum loops on the order of minutes
        packets_elapsed = (packet.seqnum - packet_prev.seqnum) & 65535
        data_us_elapsed = packets_elapsed * approx_us_per_packet

        wall_us_elapsed = packet.logged_timestamp_microseconds - packet_prev.logged_timestamp_microseconds
        if 0 == wall_us_elapsed: continue

        # this number should be 1.0 in real time and measurably higher during replay
        rate = data_us_elapsed / wall_us_elapsed

        print('rate is %g' % rate, file=sys.stderr)

main()
