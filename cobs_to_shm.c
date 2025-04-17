/*
 Copyright 2022-2025 Applied Ocean Sciences

 Permission to use, copy, modify, and/or distribute this software for any purpose with or
 without fee is hereby granted, provided that the above copyright notice and this
 permission notice appear in all copies.

 THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO
 THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT
 SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR
 ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE
 USE OR PERFORMANCE OF THIS SOFTWARE.

 Logging and fanout for datagrams arriving from serial

 This code opens a serial port (or USB CDC serial device), raises the DTR pin, ingests
 COBS-framed datagrams, de-escapes them, prepends them with a size and timestamp, logs them
 to disk, and fans them out to realtime listeners via a ring buffer in shared memory. Read
 on for details.

 For each received datagram, an eight-byte logging header is prepended, consisting of a
 little- endian unsigned 16-bit integer representing the size of the packet (not including
 the logging header), and a 48-bit little-endian unsigned integer representing the unix
 epoch time in increments of sixteen microseconds at which the packet was received.

 Up to seven bytes of padding are added after each packet to ensure that the subsequent
 header and packet remain eight-byte aligned. In downstream applications, the amount of
 padding to read and discard shall be determined by rounding the packet size indicated in
 the logging header to the next multiple of eight bytes.

 Code which consumes this logged format should therefore do an eight-byte read to ingest
 the logging header and determine the packet size, do an additional read of that size
 rounded up to the next multiple of eight, and then simply ignore the ingested padding
 bytes when processing the packet.

 On-wire packets are expected to be terminated by a zero byte, with consistent overhead
 byte stuffing (COBS) used to ensure that the frame-end byte will appear nowhere else in
 the data stream. The encoding is removed by the logger before writing the packets to disk
 and to the downstream readers.

 This code raises DTR when it opens the serial port. The upstream device should wait for
 DTR to go high before transmitting the first bytes, and reset itself upon observing DTR to
 have gone low.

 In addition to logging the packets to disk, this code makes them available in real time to
 zero or more downstream realtime readers using a ring buffer in a shared memory segment,
 allowing for zero- copy sharing of the data stream without any possibily that a slow or
 otherwise misbehaving reader can block the writer or other readers.

 Care should be taken to ensure that the system clock is synced to a GPS or precision RTC
 time reference prior to starting this code, and ideally continually therafter.
 */

/* needed for asprintf, must occur prior to any include statements */
#define _GNU_SOURCE

/* library functions */
#include "shared_memory_ringbuffer.h"

/* c standard includes */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <stdint.h>
#include <assert.h>
#include <signal.h>
#include <ctype.h>

/* posix includes */
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <termios.h>

/* useful macros */
#define WARNING_ANSI "\x1B[35;1mwarning:\x1B[0m"
#define ERROR_ANSI "\x1B[31;1merror:\x1B[0m"
#define NOPE(...) do { fprintf(stderr, ERROR_ANSI " " __VA_ARGS__); exit(EXIT_FAILURE); } while(0)
#define alloc_sprintf(...) ({ char * _tmp; if (asprintf(&_tmp, __VA_ARGS__) <= 0) abort(); _tmp ; })

static unsigned long long current_time_in_unix_microseconds(void) {
    struct timespec timespec;
    clock_gettime(CLOCK_REALTIME, &timespec);
    return timespec.tv_sec * 1000000ULL + timespec.tv_nsec / 1000UL;
}

volatile sig_atomic_t got_sigterm_or_sigint = 0;

static void sigint_handler(int sig) {
    (void)sig;
    got_sigterm_or_sigint = 1;
}

static speed_t parse_baud_rate(const unsigned long desired) {
    return (2400 == desired ? B2400 :
            4800 == desired ? B4800 :
            9600 == desired ? B9600 :
            19200 == desired ? B19200 :
            38400 == desired ? B38400 :
            57600 == desired ? B57600 :
            115200 == desired ? B115200 :
            230400 == desired ? B230400 :
#ifdef B460800
            460800 == desired ? B460800 :
#ifdef B921600
            921600 == desired ? B921600 :
#endif
#endif
            -1);
}

static int open_serial_port(const char * const path_and_maybe_baud) {
    speed_t baud = (speed_t)-1;
    char * path = strdup(path_and_maybe_baud);
    const char * const comma = strchr(path, ',');
    if (comma) {
        path[comma - path] = '\0';
        baud = parse_baud_rate(strtoul(comma + 1, NULL, 10));
        if ((speed_t)-1 == baud) NOPE("%s: baud rate %s not supported\n", __func__, comma + 1);
    }

    /* open the serial fd, non blocking (otherwise the open call itself blocks) */
    const int fd = open(path, O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (-1 == fd) NOPE("%s: %s: %s\n", __func__, path, strerror(errno));

    free(path);

#if 1
    if (-1 == fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK))
        NOPE("%s: could not fcntl(O_NONBLOCK): %s\n", __func__, strerror(errno));
#endif

    /* get the current termios settings for the serial device */
    struct termios ts;
    if (-1 == tcgetattr(fd, &ts)) NOPE("%s: tcgetattr: %s\n", __func__, strerror(errno));

    /* make as raw as possible */
    cfmakeraw(&ts);

    /* control lines ignored, and DTR will be automatically lowered when this process ends */
    ts.c_cflag |= HUPCL | CLOCAL;

    /* if input text specified a baud rate, attempt to set it */
    if (baud != (speed_t)-1)
        if (-1 == cfsetspeed(&ts, baud)) NOPE("%s: cfsetspeed(): %s\n", __func__, strerror(errno));

    /* return after 0.1 seconds if at least one byte has been received, regardless of
     whether full reads have been satisfied. note that this could in theory incur up to
     min(100 ms, packet period) of error in the timestamps prepended to packets by the
     logger, but in practice the kernel usb code seems to (almost?) always return on the
     boundary of a corresponding write by the other end. NOTE: this seems to have no effect
     when using poll() to gate the reads, but posix makes no such guarantees, so keep it */
    ts.c_cc[VMIN] = 1;
    ts.c_cc[VTIME] = 1;

    if (-1 == tcsetattr(fd, TCSANOW, &ts)) NOPE("%s: tcsetattr: %s\n", __func__, strerror(errno));

    /* attempt to clear stale data */
    if (-1 == tcflush(fd, TCIOFLUSH)) NOPE("%s: cannot tcflush: %s\n", __func__, strerror(errno));

    return fd;
}

static ssize_t readall(int fd, const void * buf, const size_t size) {
    /* loop on read() until we have written all requested bytes*/
    for (ssize_t size_read = 0; (size_t)size_read < size; ) {
        const ssize_t now = read(fd, (char *)buf + size_read, size - size_read);
        if (-1 == now) return -1;
        else if (!now) return size_read;
        size_read += now;
    }
    return size;
}

static ssize_t read_escaped_frame(void * out, const size_t max_plain_size, int fd) {
    /* note: "out" must be large enough to hold an extra final appended zero */
    unsigned char * dst = out, * plain = out;

    while (1) {
        /* read one byte */
        unsigned char code;
        if (-1 == read(fd, &code, sizeof(code))) return -1;

        /* got an end byte */
        if (0 == code) break;

        /* if we have gone too long without seeing an end byte... */
        if ((size_t)(dst - plain) + code > max_plain_size) {
            fprintf(stderr, WARNING_ANSI " %s: missing end byte\n", __func__);

            /* discard all further bytes until we see a zero byte, then reset */
            do if (-1 == read(fd, &code, sizeof(code))) return -1;
            while (code);

            return read_escaped_frame(out, max_plain_size, fd);
        }

        /* now we can do a longer read of the expected number of bytes straight into the
         output buffer, without having to escape anything or read one byte at a time, or
         worry about doing a blocking read not temporally aligned with the presence of data */
        if (-1 == readall(fd, dst, code - 1)) return -1;

        dst += code - 1;

        /* a special value of 0xff indicates that the block encodes 254 bytes */
        if (code != 0xFF) *(dst++) = 0;
    }

    return dst > plain ? (dst - plain) - 1 : 0;
}

static int text_packet(void * packet_buffer, const size_t packet_size) {
    unsigned char * restrict const byte = packet_buffer;

    /* TODO: refine this */
    size_t printable_characters = 0;
    for ( ; printable_characters < packet_size; printable_characters++) {
        if (byte[printable_characters] == '\r' ||
            byte[printable_characters] == '\n') break;
        if (!isprint(byte[printable_characters])) return 0;
    }

    if (printable_characters)
        fprintf(stderr, "%s: \"%.*s\"\n", __func__, (int)printable_characters, byte);
    return 1;
}

int main(const int argc, char ** const argv) {
    /* do some silly stuff to get a progname regardless of runtime environment */
    const char * s, * progname = argc ? ((s = strrchr(argv[0], '/')) ? s + 1 : argv[0]) : __func__;

#ifdef GIT_VERSION
    fprintf(stderr, "%s: built from commit %s\n", progname, GIT_VERSION);
#endif

    /* ensure that stdout will not be full-buffered */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* install a signal handler so that we can stop cleanly on sigint or sigterm */
    if (-1 == sigaction(SIGINT, &(struct sigaction) { .sa_handler = sigint_handler }, NULL) ||
        -1 == sigaction(SIGTERM, &(struct sigaction) { .sa_handler = sigint_handler }, NULL))
        NOPE("%s: sigaction(): %s\n", progname, strerror(errno));

    if (argc > 1) {
        fprintf(stderr, "%s: called with:", progname);
        for (size_t iarg = 1; iarg < (size_t)argc; iarg++)
            fprintf(stderr, " %s", argv[iarg]);
        fprintf(stderr, "\n");
    }

    if (argc < 2) {
        fprintf(stderr, "Usage: %s /dev/tty.usbmodem24601 [/dev/shm/]\n", argv[0]);
        fprintf(stderr, "where the optional second argument specifies the intermediate directory to which files will be written. This intermediate directory MUST NOT be in slow nonvolatile storage (such as on a microsd card) - the intention is that files will be moved to a final logging location after they are complete (and after applying compression if desired) by piping the output of %s into xargs or similar. If no second argument is given, only fanout via shm will be performed.\n", progname);
        exit(EXIT_FAILURE);
    }

    const char * escaped_serial_path = argv[1];
    const char * logging_path = argc > 2 ? argv[2] : NULL;

    if (logging_path)
        fprintf(stderr, "%s: output files will be staged in %s\n", progname, logging_path);
    else
        fprintf(stderr, "%s: logging is disabled\n", progname);

    /* todo: add some sort of check that the logging path is a tmpfs and not a microsd card */

    /* only slightly cargo cult scheduling stuff */
    if (-1 == setpriority(0, PRIO_PROCESS, -20))
        fprintf(stderr, WARNING_ANSI " %s: failed to set priority, adjust RLIMIT_NICE\n", progname);
    mlockall(MCL_CURRENT | MCL_FUTURE);

    /* logging header plus maximum size of packet, must be a multiple of 16 */
    struct {
        uint64_t logging_header;
        unsigned char packet[65528];
    } * buf = NULL;

    static_assert(!(sizeof(*buf) % 16), "max shared memory slot size must be a multiple of 16");

    /* establish a shared-memory segment into which we will place the de-escaped incoming
     packets, which allows them to be shared with zero or more listening downstream
     processes in a zero-copy scheme, with no possibility of a slow reader blocking the
     writer or other readers */
    struct shared_memory_ringbuffer * shm = shared_memory_ringbuffer_writer_init("/cobs_to_shm", 4194304, sizeof(*buf));
    if (MAP_FAILED == shm || !shm) exit(EXIT_FAILURE);

    /* sleep a bit to give simultaneously-started readers a chance to connect for determinism */
    usleep(200000);

    /* open the given path, possibly parsing a baud rate from it, in raw mode */
    const int fd_serial = open_serial_port(escaped_serial_path);

    unsigned long long time_microseconds_file_start = 0;
    FILE * fh = NULL;
    char * path = NULL;

    unsigned long long packet_time_previous = 0;

    /* loop over whole packets */
    while (1) {
        /* get the next slot in the ring buffer */
        if (!buf) buf = shared_memory_ringbuffer_acquire(shm);

        const ssize_t ret = read_escaped_frame(buf->packet, sizeof(buf->packet), fd_serial);
        if (got_sigterm_or_sigint) break;

        /* if read_escaped_frame returns -1, we either got eof or an error on the input */
        else if (-1 == ret) {
            if (ENXIO != errno)
                fprintf(stderr, "%s: %s\n", progname, strerror(errno));
            break;
        }
        else if (!ret) continue;

        const size_t packet_size = ret;
        const unsigned long long packet_time_microseconds = current_time_in_unix_microseconds();

        /* check whether a SIGINT or SIGTERM arrived before handling other errors */
        if (got_sigterm_or_sigint) {
            fprintf(stderr, "%s: breaking out of main loop due to flag\n", progname);
            break;
        }

        if (packet_time_previous > packet_time_microseconds)
            fprintf(stderr, WARNING_ANSI " %s: time has jumped backwards by %lld us, new time is %llu\n",
                    progname, packet_time_previous - packet_time_microseconds, packet_time_microseconds);
        packet_time_previous = packet_time_microseconds;

        const unsigned long long packet_time_microseconds_rounded_down_to_10s = packet_time_microseconds - (packet_time_microseconds % 10000000ULL);

        /* if rounding down gives a time later than the file start time, we need to close
         the old file and then create a new one in the next step */
        if (fh && packet_time_microseconds_rounded_down_to_10s > time_microseconds_file_start) {
            fclose(fh);
            printf("%s\n", path);
            free(path);
            fh = NULL;
        }

        /* if we just closed the most recent file or haven't opened one yet, open a new one */
        if (!fh && logging_path) {
            /* construct timestamp in ISO 8601 format, no separators, rounded down to seconds */
            struct tm unixtime_struct;
            gmtime_r(&(time_t) { packet_time_microseconds / 1000000ULL }, &unixtime_struct);
            char timestamp[17];
            strftime(timestamp, 17, "%Y%m%dT%H%M%SZ", &unixtime_struct);

            path = alloc_sprintf("%s/%s.bin", logging_path, timestamp);
            fh = fopen(path, "w");
            if (!fh) NOPE("%s: fopen(%s): %s\n", progname, path, strerror(errno));
            time_microseconds_file_start = packet_time_microseconds;
            /* would be nice to write to stderr here, but even logged writes to stderr can block */
        }

        /* populate the eight bytes we're prepending to each packet on disk and in shared memory */
        buf->logging_header = ((packet_time_microseconds / 16) << 16) | packet_size;

        /* round packet size up to the next multiple of 8, and write up to 7 bytes of
         padding, s.t. the next packet will be eight-byte-aligned within the output */
        const size_t packet_size_padded = (packet_size + 7) & ~7;

        /* zero out any padding we're going to write. note we can do this only because we
         know the shm segment enforces even more strict alignment, so if padding is
         necessary, then there is room for it at the end of the buffer slot */
        if (packet_size_padded != packet_size)
            memset(buf->packet + packet_size, 0, packet_size_padded - packet_size);

        /* done constructing unpadded portion of header and payload, release to readers */
        shared_memory_ringbuffer_send(shm, sizeof(buf->logging_header) + packet_size);

        /* write the packet to the current output file. WARNING: this should not be a file on sd */
        if (fh && !fwrite(buf, sizeof(buf->logging_header) + packet_size_padded, 1, fh))
            NOPE("%s: fwrite(): %s\n", progname, strerror(errno));

        text_packet(buf->packet, packet_size);

        const unsigned elapsed = current_time_in_unix_microseconds() - packet_time_microseconds;
        if (elapsed >= 100000)
            fprintf(stderr, WARNING_ANSI " %s: output took %u ms\n", progname, elapsed / 1000U);

        buf = NULL;
    }

    fprintf(stderr, "%s: exiting\n", progname);

    if (fh) {
        fclose(fh);
        printf("%s\n", path);
        free(path);
    }

    return 0;
}
