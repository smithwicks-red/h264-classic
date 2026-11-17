#!/bin/bash
# linux: self-contained relocatable (only avcdec_* global) + test exe
set -e; cd /home/claude/stage3/s3
U=""; for s in version init open feed_annexb decode_sample flush reset close set_skip get_delay set_delay take_lowest held peek_lowest; do U="$U -u avcdec_$s"; done
powerpc-linux-gnu-ld -r --gc-sections $U -e avcdec_init -o all_l.o ol/*.o
echo "undef: $(powerpc-linux-gnu-nm -u all_l.o | awk '{print $2}' | tr '\n' ' ')"
powerpc-linux-gnu-objcopy -w --keep-global-symbol='avcdec_*' all_l.o lib_l.o
powerpc-linux-gnu-gcc -O2 -static -I src src/t_linux.c lib_l.o -o t_linux
