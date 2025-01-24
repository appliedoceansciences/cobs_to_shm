#!/usr/bin/env python3
import sys
import wave
import socket
import numpy as np

# This provides the generator function which knows how to extract sensor-agnostic frames of
# acoustic sample data from whatever possibly sensor-specific format they are coming from
from parse_acoustic_packets import yield_acoustic_packets, yield_packet_bytes_from_log_stream

def main():
    output_length_desired = 4

    # do a bunch of boilerplate to figure out where we are getting input from
    if len(sys.argv) > 1:
        if 'shm:' in sys.argv[1]:
            try:
                from shared_memory_ringbuffer_reader import shared_memory_ringbuffer_generator
            except:
                raise RuntimeError('shared memory input not supported')

            # hack to peel off logging headers
            def yield_from_shm_and_strip_logging_header(source):
                for packet_with_logging_header in shared_memory_ringbuffer_generator(source):
                    yield packet_with_logging_header[8:]

            input_source = sys.argv[1].split(':')[1]
            yield_packet_bytes_function = yield_from_shm_and_strip_logging_header
        elif ':' in sys.argv[1]:
            address, port = sys.argv[1].split(':')
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.connect((address, int(port)))
            input_source = sock.makefile('rb')
            yield_packet_bytes_function = yield_packet_bytes_from_log_stream
            print('connected to %s:%u via tcp' % (address, int(port)), file=sys.stderr)
        else:
            input_source = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            input_source.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF, 4194304) #set the udp recv buffer to 4mb
            input_source.bind(('', int(sys.argv[1])))
            yield_packet_bytes_function = yield_packet_bytes_from_udp
            print('listening for udp on port %u' % int(sys.argv[1]), file=sys.stderr)
    else:
        print('listening for input on stdin. if udp input is desired, specify a port number to listen on. if tcp is desired, specify an address:port to connect to', file=sys.stderr)
        input_source = sys.stdin.buffer
        yield_packet_bytes_function = yield_packet_bytes_from_log_stream

    # start a generator function which will yield one acoustic packet at a time
    child = yield_acoustic_packets(yield_packet_bytes_function, input_source, None)

    # wait for the first packet
    packet = next(child, None)
    if not packet: return

    # now we can do whatever init we need to do upon having received the first packet
    fs = packet.fs
    samples_per_packet = packet.samples.shape[0]
    C = packet.samples.shape[1]

    # for now just round the output to the nearest packet
    packets_per_output = round(output_length_desired * fs / samples_per_packet)
    samples_per_output = packets_per_output * samples_per_packet

    # buffer which will hold the desired chunk of data
    output = np.zeros(dtype=packet.samples.dtype, shape=(samples_per_output, C))

    it_output = 0
    ifile = 0

    # loop over all incoming packets including the first one
    while packet is not None:
        # copy the new sample data into where it goes in the output buffer
        output[it_output:(it_output + samples_per_packet), :] = packet.samples
        it_output += samples_per_packet

        # whenever we have accumulated the desired number of samples...
        if samples_per_output == it_output:
            it_output = 0

            # write them to a .wav file in /tmp/
            filename = '/tmp/%04u.wav' % ifile
            ifile += 1
            with wave.open(filename, 'w') as w:
                w.setnchannels(C)
                w.setsampwidth(2)
                w.setframerate(fs)
                w.writeframes(output)
                w.close()

            # write the just-completed filename to stdout immediately
            print(filename, flush=True)

        # keep looping as long as there are more packets
        packet = next(child, None)

main()
