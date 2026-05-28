# `cobs_to_shm`

This repository implements the Linux end of an interface between a DAQ microcontroller and soft-realtime logging and processing environment on a Linux SBC (or Mac/Linux laptop), as well as some simple examples of how to interact with the resulting sample data, both in realtime and in postprocessing.

## Details

Consistent overhead byte stuffing (COBS) framing is used to turn a byte-oriented link (in practice a USB CDC serial device, although it will work as-is on a sufficiently fast physical serial port) into a packet-oriented link, allowing discrete acoustic and nonacoustic data packets to be interleaved and sent via the same pipe.

 This COBS framing is removed by the receiver code on the Linux SBC, and each resulting packet has a logging header prepended and, if necessary, some padding bytes appended.

The packets are fanned out to any other interested soft-realtime DSP processes via a ring buffer in shared memory. These reader processes can each come and go or misbehave in various ways without any possibility of interrupting other reader processes or logging to disk, as long as they do not starve the entire SBC of processing power or memory.

If logging is enabled, the packets are written to disk in ten-second chunks (without gaps). The filename of each completed ten-second-chunk file is written to `stdout` when each file is finished, allowing downstream logic to do something with each file (such as compress it and move it to a more permanent location, or simply delete it as in the below example). This logging can be performed either within the `cobs_to_shm` binary itself, or in a ring buffer consumer application which can be started and stopped independently.

Example soft-realtime reader code is available natively for C and Python, which reads packets from the zero-copy shared memory ring buffer. Processing in other languages is possible (at the expense of the zero-copy property) by using a stub reader process (in C or Python) which simply yields the stream of packets via its stdout, suitable for piping into a downstream or parent process implemented in another language.

## Building

Invoke `make` in this repository, with no argument, to compile the code. Optionally, invoke `make install` as root to copy the resulting binary and example `.service` files to `/usr/local/bin/` and `/etc/systemd/system/` respectively, if applicable.

## Usage

Invoke with a single argument, specifying the tty to read from:

    ./cobs_to_shm /dev/tty.usbmodem1301
    ./packet_health.py shm

Start an additional reader for logging, and pipe the output into logic which will move the resulting files to some final path:

    ./shm_logger | xargs -I file mv file /final/path/

Example `.service` files are included which invoke the `cobs_to_shm` and `shm_logger` binaries with appropriate arguments. Note that these assume a `daq` user with a sub-1000 uid (so that systemd does not delete the shm segment) whose home directory contains the destination directory for the resulting logged data. Adjust this logic according to your needs, or create a `daq` user with a sub-1000 uid and associated home directory using `useradd -rm daq`.

## Logged data

The resulting `.bin` files contain a stream of acoustic and possibly nonacoustic packets, each prefixed with an eight byte header containing a packet size and timetamp. Up to seven bytes of padding is added after each packet, if necessary, to ensure that the beginning of the next packet is aligned to eight bytes. The beginnings of the `.bin` files carry no significance and are simply aligned with wall clock time on a best-effort basis - that is, multiple consecutive `.bin` files concatenated together are also a valid `.bin` file, with no gaps. Similarly, multiple `.bin.gz` files can be concatenated together and piped through `gunzip` as if they had always been a single file.

The acoustic packets consist of a header prepended to a block of samples. The samples are signed 16-bit little endian integers, such that the various headers can be peeled off each acoustic packet and the data segments concatenated together, and the result can be interpreted as a continuous stream of PCM audio samples. The included `parse_acoustic_packets.py` script can perform this operation, as follows:

    cat /path/to/*.bin | ./parse_acoustic_packets.py > combined_raw_pcm_audio.raw

The resulting raw PCM audio can be processed as-is, or a `.wav` header can be prepended to it if necessary:

    ./prepend_wav_header.py combined_raw_pcm_audio.raw > /tmp/combined_audio.wav

Note that since `.wav` files must include the file length in the header, it is not possible to prepend a `.wav` header to a stream of data - a temporary file of not more than 4 gigabytes must first be created. If streaming operation is necessary, omit the `.wav` header and operate on just the raw PCM data.

## Components

### Standalone applications

- `cobs_to_shm`: The main application, written in C which reads from the serial device, removes the COBS framing, and fans out the resulting stream of packets to the shared memory ring buffer and optional logging

- `shm_logger`: Standalone logger that consumes packets from the ring buffer and writes them to disk using the same logic as `cobs_to_shm` itself, but which can be started and stopped independently of the former. This also serves as an example ring buffer reader application in C.

- `packet_health.py`: Debugging utility, suitable for bench testing or in-water health checks, which reads packets from the shared memory ring buffer and prints some status messages to the console. This also serves as an example ring buffer reader application in Python.

- `shm2udp.py`: Accessory utility which reads packets from the shared memory ring buffer and retransmits each one as a UDP packet to a given address and port

- `pcm2packets.py`: Bench testing utility that ingests raw PCM on `stdin`, and constructs a stream of packets on `stdout` in the same logging format emitted by `cobs_to_shm`

- `bin_to_shm`: Bench testing utility that ingests the logging format emitted by `cobs_to_shm` or `pcm2packets.py`, and populates a shared memory ring buffer just as `cobs_to_shm` would do

### Modules used by the above

- `shared_memory_ringbuffer_reader.py` and `shared_memory_ringbuffer.c`: Python and C modules with functions to read from the shared memory ring buffer and return packets one at a time to calling code. The Python module can also be run as a standalone process, and will yield the stream of packets to `stdout` in the same logging format emitted by `cobs_to_shm`.

- `parse_acoustic_packets.py`: Python module which ingests the acoustic packets and yields packets worth of samples at a time to calling code, suitable for developing soft-realtime DSP applications. Can be run as a standalone process, which will ingest the logging format emitted by `cobs_to_shm` and yield raw PCM on `stdout`, suitable for piping into `ffmpeg` or any other software which expects PCM.

## Stunts

Assuming apache2 and ffmpeg are installed, the following command (as root) can be used to serve the audio in near real-time (several seconds of latency) in human-listenable form:

    cd /var/www/html/ && shared_memory_ringbuffer_reader.py /cobs_to_shm | parse_acoustic_packets.py | ffmpeg -f s16le -ac 1 -ar 31250 -i - -af "highpass=f=30,volume=20dB" -c:a aac -b:a 128k -listen 1 -f hls -hls_flags delete_segments audio.m3u8
    
To listen to this, navigate to http://[ip of board]/audio.m3u8 in a web browser or similar.

## References

- S. Cheshire and M. Baker, "Consistent overhead byte stuffing," in IEEE/ACM Transactions on Networking, vol. 7, no. 2, pp. 159-172, April 1999, doi: 10.1109/90.769765.
