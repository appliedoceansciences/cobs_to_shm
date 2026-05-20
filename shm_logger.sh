#!/usr/bin/env bash
set -e

DESTROOT="$HOME"/element_data
mkdir -p "$DESTROOT"
cd "$DESTROOT"

DEST=$(date -u +%Y%m%dT%H%M%SZ)
ln -fns "$DEST" current

# tell stderr what we are doing and where
printf '%s: data will be written to %s/\n' $(basename "$0") $(readlink -f current) >&2

# start a child process that ingests data, logs it to disk, and writes each filename to
# stdout upon completion, whilst also serving the data via shm. the stdout of the child
# process will become the stdin of this parent script (which replaces itself with xargs)
exec < <(exec /usr/local/bin/shm_logger "$@")

# catch and ignore sigterm in all remaining processes
trap '' TERM

# the parent process will be xargs, which will ingest the filenames at their temporary
# locations in tmpfs, and compress each one and write the compressed output to the final
# location. this process and the children it spawns will ignore the initial SIGTERM and only
# exit when they see eof
exec nice -n20 xargs -I file sh -c 'mkdir -p $(readlink current) && gzip --fast -c < file > current/$(basename file).gz; rm file'
