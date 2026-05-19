#!/usr/bin/env python3

import struct
import sys

# default to 31250 sps, single channel, s16le
sample_rate = 31250
C = 1
type = 1

# loop over pairs of arguments
for key, value in zip(sys.argv[1::2], sys.argv[2::2]):
    if key == 'C': C = int(value)
    if key == 'fs': sample_rate = int(value)
    if key == 'dtype' and value == 'single': type = 3

sizeof_output_sample = 2 if type == 1 else 4

data = sys.stdin.buffer.read()
T = len(data) // (C * sizeof_output_sample)
header_size_bytes = 44
filesize_minus_eight_bytes = C * T * sizeof_output_sample + header_size_bytes - 8

header = struct.pack('4sI4s4sIHHIIHH4sI', b'RIFF', filesize_minus_eight_bytes, b'WAVE', b'fmt ', 16, type, C, sample_rate, sample_rate * C * sizeof_output_sample, C * sizeof_output_sample, sizeof_output_sample * 8, b'data', T * C * sizeof_output_sample)

sys.stdout.buffer.write(header)
sys.stdout.buffer.write(data)
