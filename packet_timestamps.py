#!/usr/bin/env python3
import sys

# This provides the generator function which knows how to extract sensor-agnostic frames of
# acoustic sample data from whatever possibly sensor-specific format they are coming from
from parse_acoustic_packets import yield_acoustic_packets, yield_packet_bytes_from_log_stream

input_source = sys.stdin.buffer
yield_packet_bytes_function = yield_packet_bytes_from_log_stream

ipacket = 0
for packet in yield_acoustic_packets(yield_packet_bytes_function, input_source, None):
    print('%u, %u.%06u, %u.%06u, %u' % (ipacket, packet.timestamp_microseconds // 1000000, packet.timestamp_microseconds % 1000000,
                                        packet.logged_timestamp_microseconds // 1000000, packet.logged_timestamp_microseconds % 1000000,
                                        packet.seqnum))
    ipacket += 1
