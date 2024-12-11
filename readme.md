# `cobs_to_disk`

For the interface between the microcontroller and soft-realtime processing environment on the Linux SBC (Raspberry Pi Zero 2W), a simple consistent overhead byte stuffing (COBS) framing scheme is used to turn the byte-oriented link (in practice a USB CDC serial device, although the software will work as-is on a physical serial port of sufficient baud rate) into a packet-oriented link, allowing discrete acoustic and nonacoustic data packets to be interleaved and sent via the same pipe.

 This COBS framing is removed by the receiver code on the Linux SBC, and each resulting packet has a logging header prepended and, if necessary, some padding bytes appended, and these are written to disk in ten-second chunks (without gaps). The filename of each completed ten-second-chunk file is written to `stdout` when each file is finished, allowing downstream logic to do something with each file (such as compress it and move it to a more permanent location).
 
 Simultaneously, the packets are fanned out to any other interested soft-realtime DSP processes via a ring buffer in shared memory. These reader processes can each come and go or misbehave in various ways without any possibility of interrupting other reader processes or the logging to disk, as long as they do not starve the entire SBC of processing power or memory.

Example soft-realtime reader code is available natively for C and Python, which reads packets from the zero-copy shared memory ring buffer. Processing in other languages is possible (at the expense of the zero-copy property) by using a stub reader process (in C) which simply yields the stream of packets via its stdout, suitable for piping into a downstream or parent process implemented in another language.

## Usage

    ./cobs_to_disk /dev/tty.usbmodem1301 | xargs rm &
    ./scope.py shm:cobs_to_disk

## References

- S. Cheshire and M. Baker, "Consistent overhead byte stuffing," in IEEE/ACM Transactions on Networking, vol. 7, no. 2, pp. 159-172, April 1999, doi: 10.1109/90.769765.
