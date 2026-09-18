#!/bin/bash
set -e

clang -Wall -Wextra -Wpedantic -O2 \
  -Isrc/motion \
  src/app/main.m \
  src/motion/imu.c \
  -framework Cocoa \
  -framework IOKit \
  -framework CoreFoundation \
  -lm \
  -o aarti

echo ""
echo "Build successful."
echo "Run with:"
echo "  sudo ./aarti"
