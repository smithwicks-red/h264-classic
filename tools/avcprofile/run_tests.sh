#!/bin/sh
# Host tests for codec-src/AVCProfile.c (any Unix with gcc).
cd "$(dirname "$0")" && gcc -std=c89 -pedantic -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer -I../../codec-src ../../codec-src/AVCProfile.c avcprofile_test.c -o "${TMPDIR:-.}/avcprofile_test" && "${TMPDIR:-.}/avcprofile_test"
