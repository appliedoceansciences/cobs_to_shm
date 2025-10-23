#!/usr/bin/env python3

import sys

from shared_memory_ringbuffer_reader import shared_memory_ringbuffer_generator
from parse_acoustic_packets import yield_acoustic_packets

def yield_from_shm_and_strip_logging_header(source):
    for packet_with_logging_header in shared_memory_ringbuffer_generator(source):
        yield packet_with_logging_header[8:]

input_source = '/cobs_to_shm'

# loop on output of above generator function, dumping samples to stdout as raw pcm
for packet in yield_acoustic_packets(yield_from_shm_and_strip_logging_header, input_source, phonemask=None):
    sys.stdout.buffer.write(packet.samples)
