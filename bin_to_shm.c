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

/* useful macros */
#define WARNING_ANSI "\x1B[35;1mwarning:\x1B[0m"
#define ERROR_ANSI "\x1B[31;1merror:\x1B[0m"
#define NOPE(...) do { fprintf(stderr, ERROR_ANSI " " __VA_ARGS__); exit(EXIT_FAILURE); } while(0)

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

    /* loop over whole packets */
    while (1) {
        /* get the next slot in the ring buffer */
        if (!buf) buf = shared_memory_ringbuffer_acquire(shm);

        uint64_t logging_header = 0;
        do {
            if (!fread(&logging_header, sizeof(uint64_t), 1, stdin)) break;
            /* if we successfully read eight bytes but they are all-bits-zero, keep reading */
        } while (!logging_header);

        const size_t packet_size = logging_header & 65535U;
        const size_t packet_size_padded = (packet_size + 7) & ~7;

        if (!fread(buf->packet, packet_size_padded, 1, stdin)) break;

        /* zero out any padding we're going to write. note we can do this only because we
         know the shm segment enforces even more strict alignment, so if padding is
         necessary, then there is room for it at the end of the buffer slot */
        if (packet_size_padded != packet_size)
            memset(buf->packet + packet_size, 0, packet_size_padded - packet_size);

        /* done constructing unpadded portion of header and payload, release to readers */
        shared_memory_ringbuffer_send(shm, sizeof(buf->logging_header) + packet_size);

        text_packet(buf->packet, packet_size);

        buf = NULL;
    }

    fprintf(stderr, "%s: exiting\n", progname);

    return 0;
}
