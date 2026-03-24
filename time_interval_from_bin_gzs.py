#!/usr/bin/env python3
# given a directory of .bin.gz files and timestamp range, emit the desired range on stdout
import sys, struct, datetime, gzip, os

def string_to_unix_time_in_microseconds(s):
    if 'T' in s and 'Z' in s:
# TODO: handle microseconds portion if present
        return datetime.datetime.timestamp(datetime.datetime.strptime(s, '%Y%m%dT%H%M%SZ').replace(tzinfo=datetime.timezone.utc)) * 1000000
    return int(s)

desired_start = None
desired_stop = None
duration = None
path = None

# loop over pairs of arguments
for key, value in zip(sys.argv[1::2], sys.argv[2::2]):
    if key == 'path': path = value
    if key == 'start': desired_start = string_to_unix_time_in_microseconds(value)
    if key == 'stop': desired_stop = string_to_unix_time_in_microseconds(value)
    if key == 'duration': duration = round(float(value) * 1e6)

if duration is not None:
    if desired_stop is not None:
        if desired_start is not None:
            raise RuntimeError('interval overspecified')
        desired_start = desired_stop - duration
    else: desired_stop = desired_start + duration

directory = os.fsencode(path)

files = []

# determine the start times of every .bin.gz file in the directory
# this can take a long time if the beginnings of every file are not yet in the page cache
for file in sorted(os.listdir(directory)):
    filename = os.fsdecode(file)
    if not '.bin.gz' in filename: continue

    with gzip.open(os.path.join(directory, file), 'r') as f:
        first_eight_bytes = f.peek(8)[0:8]

    # interpret these eight bytes as a 16-bit packet size and 48-bit timestamp
    _, timestamp_lsbs, timestamp_msbs = struct.unpack('<HHI', first_eight_bytes)

    first_packet_time = (timestamp_lsbs | (timestamp_msbs << 16)) * 16

    files.append((file, first_packet_time))

# loop over all the files
for ifile in range(len(files)):
    file, file_start = files[ifile]
    file_stop = files[ifile + 1][1] if ifile + 1 < len(files) else None

    # skip files that do not overlap with the desired range
    if desired_stop is not None and file_start > desired_stop: break
    if desired_start is not None and file_stop is not None and file_stop < desired_start: continue

    # for each file that does potentially overlap, loop over packets within it
    with gzip.open(os.path.join(directory, file), 'r') as f:
        while True:
            logging_header_bytes = f.read(8)
            if len(logging_header_bytes) == 0: break

            # interpret these eight bytes as a 16-bit packet size and 48-bit timestamp
            packet_size, timestamp_lsbs, timestamp_msbs = struct.unpack('<HHI', logging_header_bytes)

            # time of this packet in integer unix microseconds
            packet_time = (timestamp_lsbs | (timestamp_msbs << 16)) * 16

            # round packet size up to next eight bytes
            packet_size_with_padding = (packet_size + 7) & ~7

            # unconditionally read the rest of the logged packet and any padding
            packet_with_padding = f.read(packet_size_with_padding)

            # downselect before writing anything out
            if desired_stop is not None and packet_time > desired_stop: break
            if desired_start is not None and packet_time < desired_start: continue

            # read all the remaining bytes, and write the logging header and the packet out
            sys.stdout.buffer.write(logging_header_bytes + packet_with_padding)
