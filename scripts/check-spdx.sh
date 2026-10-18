#!/usr/bin/env bash
# Fails for a C++ file whose first two lines carry no SPDX identifier. Generated
# files are exempt by CONTRIBUTING.md; they are also never staged by hand, so a
# filename check is enough.
set -u

status=0
for file in "$@"; do
    case "$file" in
        */generated/*|*/build/*) continue ;;
    esac
    head -n 2 "$file" | grep -q 'SPDX-License-Identifier:' && continue
    echo "$file: missing SPDX-License-Identifier on the first two lines"
    status=1
done

exit "$status"
