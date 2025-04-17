#!/usr/bin/env python3
import sys
import numpy as np

from shared_memory_ringbuffer_reader import shared_memory_ringbuffer_generator

# This provides the generator function which knows how to extract sensor-agnostic frames of
# acoustic sample data from whatever possibly sensor-specific format they are coming from
from parse_acoustic_packets import yield_acoustic_packets, yield_packet_bytes_from_log_stream

# hack to peel off logging headers
def yield_from_shm_and_strip_logging_header(source):
    for packet_with_logging_header in shared_memory_ringbuffer_generator(source):
        yield packet_with_logging_header[8:]

def main():
    if len(sys.argv) > 1:
        input_source = sys.argv[1].split(':')[1] if len(sys.argv) > 1 and 'shm:' in sys.argv[1] else '/cobs_to_shm'
        yield_packet_bytes_function = yield_from_shm_and_strip_logging_header
    else:
        print('listening for input on stdin. if shm input is desired, pass "shm" as the sole argument', file=sys.stderr)
        input_source = sys.stdin.buffer
        yield_packet_bytes_function = yield_packet_bytes_from_log_stream

    child = yield_acoustic_packets(yield_packet_bytes_function, input_source, None)
    packet = next(child, None)
    if not packet: return

    # do any extra init that requires the first packet to have been received
    C = packet.samples.shape[1]
    fs = packet.fs
    print('%u channels, sample rate %g sps' % (C, fs), file=sys.stderr)

    while packet:
        print('          \rmin: %d, max: %d' % (np.min(packet.samples), np.max(packet.samples)),
              file=sys.stderr, end='')

        packet = next(child, None)

    print('', file=sys.stderr)

main()
