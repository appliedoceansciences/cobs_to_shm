#!/usr/bin/env python3
# given one or more concatenated bin files on stdin, and timestamp range, emit the desired range on stdout
import sys, struct, datetime

def string_to_unix_time_in_microseconds(str):
    if 'T' in str and 'Z' in str:
# TODO: handle microseconds portion if present
        return datetime.datetime.timestamp(datetime.datetime.strptime(str, '%Y%m%dT%H%M%SZ').replace(tzinfo=datetime.timezone.utc)) * 1000000
    else:
        return int(str)

start = None
stop = None
duration = None

# loop over pairs of arguments
for key, value in zip(sys.argv[1::2], sys.argv[2::2]):
    if key == 'start': start = string_to_unix_time_in_microseconds(value)
    if key == 'stop': stop = string_to_unix_time_in_microseconds(value)
    if key == 'duration': duration = float(value)

if duration is not None:
    if stop is not None:
        if start is not None:
            raise RuntimeError('interval overspecified')
        else: start = stop - duration
    else: stop = start + duration

while True:
    logging_header_bytes = sys.stdin.buffer.read(8)
    if len(logging_header_bytes) == 0: break

    # interpret these eight bytes as a 16-bit packet size and 48-bit timestamp
    packet_size, timestamp_lsbs, timestamp_msbs = struct.unpack('<HHI', logging_header_bytes)

    timestamp_us = (timestamp_lsbs | (timestamp_msbs << 16)) * 16

    # round packet size up to next eight bytes
    packet_size_with_padding = (packet_size + 7) & ~7

    # unconditionally read the rest of the logged packet and any padding
    packet_with_padding = sys.stdin.buffer.read(packet_size_with_padding)

    # downselect before writing anything out
    if stop is not None and timestamp_us > stop: break
    if start is not None and timestamp_us < start: continue

    # read all the remaining bytes, and write the logging header and the packet out
    sys.stdout.buffer.write(logging_header_bytes + packet_with_padding)
