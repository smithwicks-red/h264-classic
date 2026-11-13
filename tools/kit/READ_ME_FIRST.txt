H.264 FOR MAC OS 9 -- KIT
Decoder v15 + H264 Stream 1.2 (experimental), November 2026
=======================================================================

This plays H.264 video in QuickTime Player on a PowerPC Mac running
Mac OS 9 -- a codec that Apple only ever shipped for Mac OS X. It is the
open-source h264bsd decoder wrapped as a QuickTime decompressor.

Tested: v15 in a QEMU Mac OS 9.2.2 emulator and on a real Mac mini G4
(6-7 ms per frame at 320x240 in Millions of colours). The previous
version, v13, played on a PowerBook G3 (Pismo) with sound. v15 itself
has not yet been run on a G3.


WHAT IS IN THE KIT
-----------------------------------------------------------------------
  H264 Decoder.hqx         The decoder (a system extension). One file
                           is all there is to install.
  mp4fixer.hqx             Converts a standard .mp4 into a .mov that
                           QuickTime 6 will open, with sound. Your
                           original file is never touched.
  H264 Stream.hqx          EXPERIMENTAL. Plays an .mp4 straight from a
                           plain http:// web address (see below).
  H264 test movie.mov      A 30-second 360x240 test pattern with a
                           beeping tone, already converted. 750 frames,
                           25 fps. Made from scratch, free of rights.
  H264 Kit.dsk             Everything above, already decoded, as an HFS
                           disk image. For emulators (SheepShaver,
                           QEMU) or for writing straight to a card.
  READ ME FIRST.txt        This file.

The .hqx files are BinHex: plain text that carries a Mac file's
resource fork safely across USB sticks, CDs, e-mail and the web. They
must be decoded ON THE MAC. A copied-over raw Mac application would
arrive without its resource fork and refuse to run.


WHAT YOU NEED
-----------------------------------------------------------------------
  - A PowerPC Mac with Mac OS 9.2.2. (Developed under 9.2.2. Earlier
    versions are untested.)
  - QuickTime 6.0.3, the last QuickTime for Mac OS 9. Earlier QuickTime
    is untested.
  - StuffIt Expander, which comes with Mac OS 9.
  - H.264 video in the BASELINE profile. Main and High profile are not
    supported (no CABAC, no B-frames). The Internet Archive's "512Kb
    MPEG4" files, YouTube's old 360p MP4 (format 18) and many older
    phones' clips are Baseline; video from a modern phone is usually
    High profile and will not play.


INSTALLING
-----------------------------------------------------------------------
  0. UPGRADING from a September 2026 kit? First drag BOTH "H264 Decoder"
     and "H264 Auto-Register" out of System Folder > Extensions to the
     Trash, and restart. From a v14 kit: just replace "H264 Decoder".

  1. Copy the kit to the Mac's own hard disk first, then drag the .hqx
     files onto StuffIt Expander (or double-click them). (Decoding
     straight onto a network or shared volume is best avoided -- in the
     emulator, StuffIt Expander froze the machine doing exactly that.)

  2. Drag "H264 Decoder" onto the CLOSED System Folder icon. The Finder
     says it "needs to be stored in the Extensions folder" -- click OK.

  3. Restart.

Nothing visible happens at startup. That is correct: QuickTime finds
the decoder in the Extensions folder by itself.


PLAYING
-----------------------------------------------------------------------
  - Open "H264 test movie.mov" in QuickTime Player and press Play.
  - Your own .mp4 files: open mp4fixer, choose the .mp4, save the .mov
    it suggests, then open that .mov in QuickTime Player. Type q and
    Return in mp4fixer's window to quit it.

Why the conversion: QuickTime 6's MPEG-4 importer predates H.264 and
throws away the part of an .mp4 the decoder needs (error -8971).
mp4fixer rewrites the file so QuickTime's movie parser reads it instead,
and rewrites the AAC sound description so the sound plays too.


H264 STREAM (EXPERIMENTAL)
-----------------------------------------------------------------------
H264 Stream plays an .mp4 from the web without downloading it first.
Double-click it, type or paste an http:// address of an .mp4 (H.264
video, AAC sound) and press Return. It fetches only the file's index
(usually a few hundred KB), checks that the video is Baseline, writes a
small "reference movie" to the Desktop and asks QuickTime Player to
open it. QuickTime then streams the picture and sound itself as it
plays, so the progress bar fills gradually, like any web movie.

Tested with the Internet Archive's "512Kb MPEG4" files, for example:
  http://archive.org/download/Popeye_forPresident/Popeye_forPresident_512kb.mp4

Limits, plainly:
  - PLAIN HTTP ONLY. QuickTime 6 cannot fetch https://, and most of
    the web is now https-only. If a server sends H264 Stream to an
    https address it stops and says "This item needs HTTPS".
  - Baseline profile only, as above. A High-profile file is refused
    before QuickTime Player opens.
  - Everything it does is written to "H264 Stream Log" next to the
    application. Send that file with any report.
  - While it is fetching it does not answer Quit; wait for it to finish.


THE DECODER'S REPORT (OPTIONAL)
-----------------------------------------------------------------------
The decoder writes a report only if you ask for one: it never creates
the file itself.

  1. Open SimpleText, choose File > Save without typing anything, and
     save the empty document on the DESKTOP as exactly

         H264 Decoder Log

  2. Play the test movie from start to end, then quit QuickTime Player.
     The decoder appends a report to that file each time a movie
     closes.

  3. To make the decoder silent again, throw the file away.

The report answers "can this Mac keep up?":

  STATS pictures shown           750 means every frame of the test
                                 movie was shown. Fewer means QuickTime
                                 skipped frames to keep up.
  STATS blits to y420/2vuy/32-bit RGB
                                 Which drawing path QuickTime chose.
  STATS avg us per call          Time to decode AND draw one frame, in
                                 microseconds. Real time for this clip
                                 is 40000 or less.
  STATS max us per call          The slowest single frame.
  STATS playback run fps x100    Frames per second while playing,
                                 times 100. 2500 is full speed.
  LATE calls QuickTime flagged catch-up
                                 Frames QuickTime handed over only to
                                 catch up after falling behind.
  LATE pictures decoded but not painted
                                 Late frames decoded but not drawn, to
                                 save time (never more than two in a
                                 row).
  STATS decode errors survived   Damaged frames skipped. Should be 0
                                 for the test movie.

Try it in both Millions and Thousands of colours (Monitors control
panel): QuickTime draws by a different path in each, and on a G3 either
one could be the faster.


UNINSTALLING
-----------------------------------------------------------------------
Drag "H264 Decoder" out of the Extensions folder and restart. Holding
Shift at startup (extensions off) disables it.


IF SOMETHING GOES WRONG
-----------------------------------------------------------------------
  - QuickTime says a compressor is missing: the decoder is not in the
    Extensions folder, or the Mac was not restarted after installing.
  - "Error -8971" when opening a movie: the file was not converted with
    mp4fixer.
  - The movie opens but the picture is black: make the "H264 Decoder
    Log" file as described above, play again, and send it.
  - Sound but a blank picture on a Main or High profile file: not
    supported. The log (if you made one) says "UNSUPPORTED H.264
    stream" and the profile number (77 Main, 100 High).
  - A freeze: note what was on screen, restart with Shift held, and
    send the H264 Decoder Log if there is one.


The decoder is h264bsd (Apache License 2.0, with MIT-licensed wrapper
code), ported with CodeWarrior Pro 8.3 under Mac OS 9.2.2. The port
itself is MIT-licensed. Built with AI assistance. Source and the full
story, including every dead end, are in the project's repository.
