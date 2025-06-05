#!/usr/bin/env python3

import sys
import socket

from shared_memory_ringbuffer_reader import shared_memory_ringbuffer_generator

if len(sys.argv) < 2:
    print('usage: %s ip:port [shm:/shm]' % sys.argv[0])
    sys.exit(1)

udp_dest_host, udp_dest_port = sys.argv[1].split(':')
udp_dest_port = int(udp_dest_port)

shm_name = sys.argv[2].split(':')[2] if len(sys.argv) > 2 and 'shm:' in sys.argv[2] else '/cobs_to_shm'

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

# loop over packets yielded from shm ring buffer reader, stripping off the eight byte header
for packet_with_logging_header in shared_memory_ringbuffer_generator(shm_name):
    sock.sendto(packet_with_logging_header[8:], (udp_dest_host, udp_dest_port))
