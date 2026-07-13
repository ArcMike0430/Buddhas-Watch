#!/usr/bin/env bash
# setup_esp_idf.sh — Install ESP-IDF toolchain for ESP32-S3
set -e

echo "=== ESP-IDF Setup for Buddhas-Watch ==="

# Prerequisites
sudo apt install -y git wget flex bison gperf python3 python3-pip \
    python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util \
    libusb-1.0-0

# Clone ESP-IDF
if [ ! -d ~/esp/esp-idf ]; then
    mkdir -p ~/esp
    git clone --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
fi

cd ~/esp/esp-idf
./install.sh esp32s3
. ./export.sh

echo "=== Done ==="
echo "To build: cd firmware/esp32-s3 && idf.py set-target esp32s3 && idf.py build"
