#!/usr/bin/env bash
# Build and run tests/test_card_tasks.c: the card's state machine (main/card_app.c) and its ClawHunt
# client (main/cloud.c) on a computer, over the stand-in platform of tests/card_stubs.
#
#   CJSON_DIR=<esp-idf>/components/json/cJSON tools/card_host_tests.sh [answers.jsonl ...]
#
# 1. "checks": what the card does with what the server tells it about its agent's tasks.
# 2. "usb": one script of USB messages and keys, run through this tree's card_app.c and through the
#    card_app.c of the release before the server spoke of tasks (0.7.6). Both must print the same:
#    the USB messages work as before.
# 3. For each answers file given (a real server's answers in the order one card applied them, as
#    written by the backend's local walk): the card's client is run against them and "screen ..."
#    lines say what the holder saw. A file named X.jsonl with X.screen beside it is compared with it.
#
# Needs a C compiler and cJSON's two source files; nothing from ESP-IDF is compiled besides them.
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

: "${CJSON_DIR:?set CJSON_DIR to the directory with cJSON.c and cJSON.h (ESP-IDF: components/json/cJSON)}"
# The release before this one. CARD_APP_OLD_FILE can name a copy of that file instead of asking git; where git
# does not have that release (a copy of the repository without the firmware's history), tests/card_app_old.c is it.
old_ref="${CARD_APP_OLD_REF:-d7aae17}"
out="$(mktemp -d /tmp/card-host-tests.XXXXXX)"
trap 'case "${out}" in /tmp/card-host-tests.*) rm -rf -- "${out}" ;; esac' EXIT

flags=(-std=gnu11 -Wall -Wextra -Itests/card_stubs -Imain "-I${CJSON_DIR}")
sources=(tests/test_card_tasks.c main/cloud.c main/card_model.c "${CJSON_DIR}/cJSON.c")

"${CC:-cc}" "${flags[@]}" "${sources[@]}" -o "${out}/new"
"${out}/new" checks

if [[ -n "${CARD_APP_OLD_FILE:-}" ]]; then
    cp -- "${CARD_APP_OLD_FILE}" "${out}/card_app_old.c"
elif git cat-file -e "${old_ref}:main/card_app.c" 2> /dev/null; then
    git show "${old_ref}:main/card_app.c" > "${out}/card_app_old.c"
else
    # A copy of the repository without the firmware's history: that release's file is kept beside the tests.
    cp -- tests/card_app_old.c "${out}/card_app_old.c"
fi
# The old file knows nothing of the newer values in card_model.h: its switches warn, which is expected.
"${CC:-cc}" "${flags[@]}" -Wno-switch -DCARD_APP_OLD "-DCARD_APP_SOURCE=\"${out}/card_app_old.c\"" "${sources[@]}" -o "${out}/old"
"${out}/old" usb > "${out}/usb-old.txt"
"${out}/new" usb > "${out}/usb-new.txt"
if ! diff -u "${out}/usb-old.txt" "${out}/usb-new.txt"; then
    echo "USB script: the card no longer does what ${old_ref} did" >&2
    exit 1
fi
echo "USB script: $(wc -l < "${out}/usb-new.txt") lines, the same as ${old_ref}"

for answers in "$@"; do
    "${out}/new" replay "${answers}" | tee "${out}/replay.txt"
    expected="${answers%.jsonl}.screen"
    if [[ -f "${expected}" ]]; then
        if ! diff -u <(tr -d '\r' < "${expected}") <(grep '^screen ' "${out}/replay.txt" | sed 's/^screen //'); then
            echo "replay of ${answers}: the card showed something else than ${expected}" >&2
            exit 1
        fi
        echo "replay of ${answers}: the card showed what ${expected} says"
    fi
done
echo "Card host tests: PASS"
