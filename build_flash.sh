#!/usr/bin/env bash
# Build otto-robot and flash it to the board over /dev/ttyUSB0.
# Usage: ./build_flash.sh
set -e

PORT=/dev/ttyUSB0

source /home/ahmad/.espressif/python_env/idf6.0_py3.10_env/bin/activate
source /home/ahmad/esp/v6.0.2/esp-idf/export.sh

python3 scripts/build.py otto-robot --name otto-robot --language en-US --wake-word wn9_jarvis_tts

fuser -k "$PORT" 2>/dev/null || true
sleep 1

idf.py -p "$PORT" flash
