#!/usr/bin/env bash
set -e

DESTROOT="$HOME"/element_data
mkdir -p "$DESTROOT"
cd "$DESTROOT"

DEST=$(date -u +%Y%m%dT%H%M%SZ)
ln -fns "$DEST" current

# tell stderr what we are doing and where
printf '%s: data will be written to %s/\n' $(basename "$0") $(readlink -f current) >&2

# exec the main process binary, and pipe its stdout to the stdin of a child process
# which will ingest the filenames at their temporary locations in tmpfs, and compress
# each one and write the compressed output to the final location. this child process
# and its own children will ignore the initial SIGTERM and only exit when they see eof,
# such that on service termination, no temporary files will remain in tmpfs
exec /usr/local/bin/shm_logger "$@" > >(trap '' TERM; exec nice -n20 xargs -I file sh -c 'mkdir -p $(readlink current) && gzip --fast -c < file > current/$(basename file).gz; rm file')
