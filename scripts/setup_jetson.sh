#!/usr/bin/env bash
# setup_jetson.sh — Install Buddhas-Watch dependencies on Jetson Orin Nano
set -e

echo "=== Buddhas-Watch Jetson Setup ==="

# System dependencies
sudo apt update
sudo apt install -y python3-pip python3-numpy python3-scipy libsndfile1

# Python packages
pip3 install numpy scipy sounddevice qiskit qiskit-aer-gpu

# Optional: CUDA quantum acceleration
# pip3 install cuquantum-python

echo "=== Checking CUDA ==="
nvcc --version || echo "WARNING: CUDA not found — quantum simulators will be CPU-only"

echo "=== Done ==="
echo "Run: cd python/csi_monitor && python csi_defense.py"
