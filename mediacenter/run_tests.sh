#!/bin/sh
cd "$(dirname "$0")" && gcc -std=c89 -pedantic -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer mc_core.c mc_test.c -o "${TMPDIR:-.}/mc_test" && "${TMPDIR:-.}/mc_test" fixtures
