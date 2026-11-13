/*
    H264CodecDispatch.h  --  component selector dispatch table

    ------------------------------------------------------------------
    Why this file has to exist at all
    ------------------------------------------------------------------
    ComponentDispatchHelper.c requires COMPONENT_DISPATCH_FILE to name a
    header it can #include -- twice, with two different sets of macro
    definitions -- to build (a) the per-range arrays of
    {function pointer, MixedMode ProcInfo} pairs and (b) the small table
    of ranges that indexes them.

    Apple ships no ready-made "ImageCodecDispatch.h". The
    ComponentIncludes folder in this CodeWarrior install holds exactly 16
    files, all of them either "<Manager>.k.h" prototype headers or
    ComponentDispatchHelper.c itself. Every component of this era wrote
    its own dispatch table and named it after itself; this is ours.

    Because the file is #included twice with the same macro names meaning
    different things each time, it must contain ONLY these macro
    invocations -- no #ifndef guard, no declarations, nothing else. A
    normal include guard would make the second inclusion expand to
    nothing and silently produce an empty range table, which compiles
    fine and then fails at runtime.

    ------------------------------------------------------------------
    How the selector -> function lookup actually works
    ------------------------------------------------------------------
    Verbatim from COMPONENTSELECTORLOOKUP in ComponentDispatchHelper.c
    (read out of the real file on 2026-09-09, not from memory):

        theRange = selector_num >> RangeShift;
        if (theRange < 0) {
            selector_num += SelOffset;
            if (selector_num >= 0) {
                infoP = &(cdhRangeDispatch[0].cdhDispatchInfoP)[selector_num];
            }
        } else {
            if (theRange < RangeCount) {
                selector_num &= RangeMask;
                if (selector_num < cdhRangeDispatch[theRange+1].rangeMax)
                    infoP = &(cdhRangeDispatch[theRange+1].cdhDispatchInfoP)[selector_num];
            }
        }

    Read that negative branch twice. It checks the LOW end only. There is
    NO upper-bound check on the negative range -- no comparison against
    rangeMax the way the positive branch has. If SelOffset is larger than
    the number of entries you list, the lookup indexes off the end of the
    array, picks up whatever two words follow it in static data, and then
    either calls that as a function pointer or returns it as a result.
    Nothing warns you. The component links, registers, and then fails at
    the first Open with no diagnostic anywhere.

    That makes the relationship between SelOffset and the entry count the
    single most dangerous thing in this file, so it is now nailed down by
    construction rather than by trusting a header constant.

    ------------------------------------------------------------------
    Why SelOffset is a literal 12 and not -kComponentGetPublicResourceSelect
    ------------------------------------------------------------------
    The previous version wrote:

        ComponentSelectorOffset(-kComponentGetPublicResourceSelect)

    with nine entries below it. That is self-documenting and it is also a
    gamble, because it silently assumes the SDK's most-negative standard
    selector is exactly -9. Which value it actually holds depends on the
    Universal Interfaces version:

        UI 3.4.1 / QuickTime 5   ...GetMPWorkFunction = -8
                                 ...GetPublicResource = -9
        later SDKs               ...GetMPWorkFunction = -8
                                 ...ExecuteWiredAction = -9
                                 ...GetPublicResource = -10

    Components.k.h in THIS install (UI 3.4.1) declares nine standard
    calls and no ExecuteWiredAction, which points at -9 -- but "points
    at" is not "is", and the failure mode if it is -10 is the silent
    out-of-bounds read described above, landing exactly on Open.

    The fix is to stop caring. The only thing the helper does with
    SelOffset is `index = selector + SelOffset`, so a table is correct if
    and only if its LAST entry is selector -1 and it has SelOffset
    entries counting backwards from there. That relationship is entirely
    internal to this file. Pick SelOffset = 12, list 12 entries ending at
    Open, and every selector from -1 to -12 lands where it should no
    matter what any header says. The three leading slots cover -12, -11
    and -10, which are either unassigned or GetPublicResource depending
    on the SDK; all three are errors either way, so being unsure which is
    which costs nothing.

    ComponentError(X) expands to `kCOMPONENT_ERROR, 0,` and never
    references a symbol named X, so the placeholder names below are pure
    documentation and cannot fail to link.

    ------------------------------------------------------------------
    Which architecture this table encodes
    ------------------------------------------------------------------
    There are two ways to write a QuickTime decompressor:

      (A) Self-contained decompressor  <-- WHAT WE DO
          Implement PreDecompress (0x0005) and BandDecompress (0x0006)
          and do the work ourselves. Both take CodecDecompressParams,
          which carries `data`, `bufferSize` and the destination
          `dstPixMap`. No delegate component, no extra plumbing.

      (B) Base-codec client / sub-codec
          Delegate 0x0005 and 0x0006 to an instance of Apple's base
          decompressor ('base'), which then calls back into
          Preflight (0x0200), Initialize (0x0201), BeginBand (0x0202),
          DrawBand (0x0203), EndBand (0x0204). Those take
          ImageSubCodecDecompressRecord, a *different* struct that has
          baseAddr/rowBytes/codecData but deliberately no bufferSize.

    We chose (A): the code was already shaped for it, it needs no
    delegate component, and with nothing between us and the destination
    PixMap it is far easier to debug the first real decode. If we ever
    want the base codec's buffer management, the migration is to move
    BandDecompress's body into DrawBand, PreDecompress's setup into
    Preflight, and flip the entries below.

    StdComponentCall vs ComponentCall differ only in which ProcInfo
    constant they glue the name onto (uppCallComponentXProcInfo vs
    uppImageCodecXProcInfo), so the standard Component Manager calls must
    use StdComponentCall and the codec calls must use ComponentCall.
*/

ComponentSelectorOffset(12)

ComponentRangeCount(3)
ComponentRangeShift(8)
ComponentRangeMask(FF)

/* ---- Negative range: standard Component Manager selectors ----
   Twelve entries, listed most-negative first, ending at Open (-1).
   index = selector + 12, so Open lands on the last entry. Do not add or
   remove an entry here without changing ComponentSelectorOffset above by
   the same amount -- the helper does not bounds-check this block. */
ComponentRangeBegin(0)
    ComponentError(cdhUnassigned12)         /* -12 unassigned in every SDK   */
    ComponentError(cdhUnassigned11)         /* -11 unassigned in every SDK   */
    ComponentError(cdhGetPublicResourceOr10)/* -10 GetPublicResource (newer) */
    ComponentError(cdhWiredActionOrPubRes9) /* -9  GetPublicResource (UI 3.4.1)
                                                   or ExecuteWiredAction     */
    ComponentError(GetMPWorkFunction)       /* -8  no MP work function       */
    ComponentError(Unregister)              /* -7                            */
    StdComponentCall(Target)                /* -6                            */
    ComponentError(Register)                /* -5                            */
    StdComponentCall(Version)               /* -4                            */
    StdComponentCall(CanDo)                 /* -3  generated by the helper   */
    StdComponentCall(Close)                 /* -2                            */
    StdComponentCall(Open)                  /* -1                            */
ComponentRangeEnd(0)

/* ---- Range 0 (0x0000-0x002B): the image codec calls ----
   The three we implement are GetCodecInfo, PreDecompress and
   BandDecompress. Everything else returns badComponentSelector: the
   compression-side calls because we are a decoder only and claiming
   otherwise would surface as a confusing failure much later, and the
   settings/parameter-dialog calls because we expose no user settings.

   Entries are positional -- index = selector & 0xFF -- so the padding
   entries at 0x26, 0x27 and 0x2A are load-bearing. ImageCodec.h simply
   defines no selector at those three numbers (it jumps 0x25 -> 0x28 and
   0x29 -> 0x2B), and without placeholders every later entry would shift
   up and misdispatch. This range IS bounds-checked by the helper. */
ComponentRangeBegin(1)
    ComponentCall(GetCodecInfo)                     /* 0x00 */
    ComponentError(GetCompressionTime)              /* 0x01  compress-side */
    ComponentError(GetMaxCompressionSize)           /* 0x02  compress-side */
    ComponentError(PreCompress)                     /* 0x03  compress-side */
    ComponentError(BandCompress)                    /* 0x04  compress-side */
    ComponentCall(PreDecompress)                    /* 0x05  <-- ours */
    ComponentCall(BandDecompress)                   /* 0x06  <-- ours */
    ComponentError(Busy)                            /* 0x07  we are synchronous */
    ComponentError(GetCompressedImageSize)          /* 0x08 */
    ComponentError(GetSimilarity)                   /* 0x09 */
    ComponentError(TrimImage)                       /* 0x0A */
    ComponentError(RequestSettings)                 /* 0x0B */
    ComponentError(GetSettings)                     /* 0x0C */
    ComponentError(SetSettings)                     /* 0x0D */
    ComponentError(Flush)                           /* 0x0E */
    ComponentError(SetTimeCode)                     /* 0x0F */
    ComponentError(IsImageDescriptionEquivalent)    /* 0x10 */
    ComponentError(NewMemory)                       /* 0x11 */
    ComponentError(DisposeMemory)                   /* 0x12 */
    ComponentError(HitTestData)                     /* 0x13 */
    ComponentError(NewImageBufferMemory)            /* 0x14 */
    ComponentError(ExtractAndCombineFields)         /* 0x15 */
    ComponentError(GetMaxCompressionSizeWithSources)/* 0x16  compress-side */
    ComponentError(SetTimeBase)                     /* 0x17 */
    ComponentError(SourceChanged)                   /* 0x18 */
    ComponentError(FlushFrame)                      /* 0x19 */
    ComponentError(GetSettingsAsText)               /* 0x1A */
    ComponentError(GetParameterListHandle)          /* 0x1B */
    ComponentError(GetParameterList)                /* 0x1C */
    ComponentError(CreateStandardParameterDialog)   /* 0x1D */
    ComponentError(IsStandardParameterDialogEvent)  /* 0x1E */
    ComponentError(DismissStandardParameterDialog)  /* 0x1F */
    ComponentError(StandardParameterDialogDoAction) /* 0x20 */
    ComponentError(NewImageGWorld)                  /* 0x21 */
    ComponentError(DisposeImageGWorld)              /* 0x22 */
    ComponentError(HitTestDataWithFlags)            /* 0x23 */
    ComponentError(ValidateParameters)              /* 0x24 */
    ComponentError(GetBaseMPWorkFunction)           /* 0x25 */
    ComponentError(unusedSelector26)                /* 0x26  padding, see above */
    ComponentError(unusedSelector27)                /* 0x27  padding, see above */
    ComponentError(RequestGammaLevel)               /* 0x28 */
    ComponentError(GetSourceDataGammaLevel)         /* 0x29 */
    ComponentError(unusedSelector2A)                /* 0x2A  padding, see above */
    ComponentError(GetDecompressLatency)            /* 0x2B */
ComponentRangeEnd(1)

/* ---- Range 1 (0x0100-0x0102): SMPTE effect calls. We are not an effect. ---- */
ComponentRangeUnused(2)

/* ---- Range 2 (0x0200-0x0209): base-codec callbacks. Architecture (B)
        only; unused here because nothing delegates to us. ---- */
ComponentRangeUnused(3)
