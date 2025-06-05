# `cobs_to_shm`

This repository implements the Linux end of an interface between a DAQ microcontroller and soft-realtime logging and processing environment on a Linux SBC (or Mac/Linux laptop), as well as some simple examples of how to interact with the resulting sample data, both in realtime and in postprocessing.

## Details

Consistent overhead byte stuffing (COBS) framing is used to turn a byte-oriented link (in practice a USB CDC serial device, although it will work as-is on a sufficiently fast physical serial port) into a packet-oriented link, allowing discrete acoustic and nonacoustic data packets to be interleaved and sent via the same pipe.

 This COBS framing is removed by the receiver code on the Linux SBC, and each resulting packet has a logging header prepended and, if necessary, some padding bytes appended, and these are written to disk in ten-second chunks (without gaps). The filename of each completed ten-second-chunk file is written to `stdout` when each file is finished, allowing downstream logic to do something with each file (such as compress it and move it to a more permanent location, or simply delete it as in the below example).
 
 Simultaneously, the packets are fanned out to any other interested soft-realtime DSP processes via a ring buffer in shared memory. These reader processes can each come and go or misbehave in various ways without any possibility of interrupting other reader processes or the logging to disk, as long as they do not starve the entire SBC of processing power or memory.

Example soft-realtime reader code is available natively for C and Python, which reads packets from the zero-copy shared memory ring buffer. Processing in other languages is possible (at the expense of the zero-copy property) by using a stub reader process (in C or Python) which simply yields the stream of packets via its stdout, suitable for piping into a downstream or parent process implemented in another language.

## Usage

Invoke with one argument for no logging:

    ./cobs_to_shm /dev/tty.usbmodem1301
    ./packet_health.py shm

Invoke with two arguments for logging, and pipe the output into logic which will move the resulting files to some final path:

    ./cobs_to_shm /dev/tty.usbmodem1301 /dev/shm/ | xargs -I file mv file /final/path/

## Components

### Standalone applications

- `cobs_to_shm`: The main application, written in C which reads from the serial device, removes the COBS framing, and fans out the resulting stream of packets to the shared memory ring buffer and optional logging

- `packet_health.py`: Debugging utility, suitable for bench testing or in-water health checks, which reads packets from the shared memory ring buffer and prints some status messages to the console

- `shm2udp.py`: Accessory utility which reads packets from the shared memory ring buffer and retransmits each one as a UDP packet to a given address and port

- `pcm2packets.py`: Bench testing utility that ingests raw PCM on `stdin`, and constructs a stream of packets on `stdout` in the same logging format emitted by `cobs_to_shm`

- `bin_to_shm`: Bench testing utility that ingests the logging format emitted by `cobs_to_shm` or `pcm2packets.py`, and populates a shared memory ring buffer just as `cobs_to_shm` would do

### Modules used by the above

- `shared_memory_ringbuffer_reader.py` and `shared_memory_ringbuffer.c`: Python and C modules which connects to the shared memory ring buffer and yields the packets one at a time to calling code. The Python module can also be run as a standalone process, and will yield the stream of packets to `stdout` in the same logging format emitted by `cobs_to_shm`.

- `parse_acoustic_packets.py`: Python module which ingests the acoustic packets and yields packets worth of samples at a time to calling code, suitable for developing soft-realtime DSP applications. Can be run as a standalone process, which will ingest the logging format emitted by `cobs_to_shm` and yield raw PCM on `stdout`, suitable for piping into `ffmpeg` or any other software which expects PCM.

## References

- S. Cheshire and M. Baker, "Consistent overhead byte stuffing," in IEEE/ACM Transactions on Networking, vol. 7, no. 2, pp. 159-172, April 1999, doi: 10.1109/90.769765.
