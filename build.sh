#!/usr/bin/env bash
# Build the card's firmware. The application image lands in dist/, named after the firmware's version.
#
#   ./build.sh            where ESP-IDF 5.5.3 is set up (idf.py on the PATH); otherwise in Docker
#   ./build.sh --docker   in Espressif's ESP-IDF 5.5.3 container, whatever this shell has
#
# On Windows: `./build.sh --docker` from Git Bash with Docker Desktop, or build.ps1 with a local ESP-IDF.
# A firmware/build folder made on another machine or by the other way of building has to go first (the two
# do not agree about where things are).
#
# The version is CARD_FW_VERSION in firmware/main/card_model.h. It is also written into the image, so
# `python tools/flash.py check` can say which firmware a card runs.
# The image is the application only: it is written to 0x10000 by tools/flash.py, never from 0x0.
set -euo pipefail

case "${1:-}" in
    "" | --docker) ;;
    *)
        echo "usage: ./build.sh [--docker]" >&2
        exit 2
        ;;
esac

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "${root}"

# Espressif's ESP-IDF 5.5.3 container, by the digest it had on 2026-11-24 (Docker Hub, pushed 2026-02-18; the
# digest is of the image list, so it serves amd64 and arm64). A tag can be moved to another image; a digest cannot.
idf_image="espressif/idf:v5.5.3@sha256:8ccd4d2ce413889c6c2bba57e986c670302094efb91c913c6091152e317a7805"

version="$(awk -F'"' '/^#define CARD_FW_VERSION/ {print $2}' firmware/main/card_model.h)"
if [[ ! "${version}" =~ ^[0-9]+[.][0-9]+[.][0-9]+$ ]]; then
    echo "no version like 1.2.3 found as CARD_FW_VERSION in firmware/main/card_model.h" >&2
    exit 1
fi

if [[ "${1:-}" == "--docker" ]] || ! command -v idf.py > /dev/null 2>&1; then
    if ! command -v docker > /dev/null 2>&1; then
        echo "neither idf.py nor docker is here: install Docker, or set up ESP-IDF 5.5.3 (Espressif's Get Started guide)" >&2
        exit 1
    fi
    echo "building firmware ${version} in ${idf_image}"
    # Git Bash on Windows rewrites arguments that look like paths (/project) before docker sees them: not these
    export MSYS_NO_PATHCONV=1
    # the container runs as root, so on Linux the files it leaves in firmware/build belong to root
    docker run --rm -e IDF_TARGET=esp32c3 -v "${root}/firmware:/project" -w /project "${idf_image}" \
        /bin/bash -c "git config --global --add safe.directory '*' && idf.py -DPROJECT_VER=${version} build"
else
    idf_version="$(idf.py --version 2>&1 || true)"
    if [[ "${idf_version}" != *5.5.3* ]]; then
        echo "this firmware is built with ESP-IDF 5.5.3; the idf.py on the PATH says: ${idf_version}" >&2
        exit 1
    fi
    echo "building firmware ${version} with ${idf_version} ($(command -v idf.py))"
    (cd firmware && idf.py "-DPROJECT_VER=${version}" build)
fi

mkdir -p dist
name="clawhunt-agent-card-${version}-app.bin"
cp firmware/build/FoloToy-AI-Passport.bin "dist/${name}"
# one line, "<sha256>  <name>", the same whichever tool made the sum and whichever script wrote the file
if command -v sha256sum > /dev/null 2>&1; then
    sum="$(sha256sum "dist/${name}" | cut -d' ' -f1)"
else
    sum="$(shasum -a 256 "dist/${name}" | cut -d' ' -f1)"
fi
printf '%s  %s\n' "${sum}" "${name}" > dist/SHA256SUMS.txt
cat dist/SHA256SUMS.txt
echo "built dist/${name}"
echo "next: python tools/flash.py flash dist/${name}"
