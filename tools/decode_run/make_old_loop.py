"""
make_old_loop.py NEW.c OLD.c -- reconstruct the pre-2026-09-23 decode loop
(the one that broke out on readBytes == 0) inside an otherwise current
H264Codec.c, so decode_run can measure old vs new on identical code.
"""
import sys
src = open(sys.argv[1], encoding='latin-1').read()
start = src.index('    {\n        u32 offset = 0;\n        int stalls = 0;')
end = src.index('            offset += readBytes;\n        }\n    }', start) + len('            offset += readBytes;\n        }\n    }')
old_loop = '''    {
        u32 offset = 0;

        while (offset < annexBSize)
        {
            readBytes    = 0;
            decodeStatus = h264bsdDecode(glob->h264,
                                          glob->annexBScratch + offset,
                                          annexBSize - offset,
                                          0, &readBytes);

            if (decodeStatus == H264BSD_ERROR ||
                decodeStatus == H264BSD_MEMALLOC_ERROR)
            {
                return internalComponentErr;
            }

            if (readBytes == 0) break;   /* no progress -- never spin */
            offset += readBytes;

            if (decodeStatus == H264BSD_PIC_RDY)
                DrainOutputPictures(glob, params);
        }
    }'''
out = src[:start] + old_loop + src[end:]
# ...and drop the repeat-last-picture fallback, which the old code lacked
rb = out.find('    /* Every call must leave a picture in the destination.')
if rb >= 0:
    re_ = out.index('    return noErr;', rb)
    out = out[:rb] + out[re_:]
open(sys.argv[2], 'w', encoding='latin-1').write(out)
print("old loop reconstructed into", sys.argv[2])
