/* Minimal Classic Mac OS declarations for a host SYNTAX check of
   H264Startup.c. 32-bit widths where it matters; signatures follow
   Universal Interfaces 3.4 as closely as memory allows -- CodeWarrior
   against the real headers remains the final word. */
#ifndef MACSTUB_H
#define MACSTUB_H
typedef short OSErr; typedef unsigned char Boolean; typedef char *Ptr; typedef Ptr *Handle;
typedef long Size; typedef unsigned long OSType; typedef unsigned char Str255[256];
typedef unsigned char *StringPtr; typedef const unsigned char *ConstStr255Param;
typedef const unsigned char *ConstStr63Param; typedef unsigned long UInt32;
#define true 1
#define false 0
#define noErr 0
#define fnfErr (-43)
#define smSystemScript (-1)
#define fsWrPerm 2
#define fsFromStart 1
typedef struct { short vRefNum; long parID; Str255 name; } FSSpec;
OSErr FSMakeFSSpec(short, long, ConstStr255Param, FSSpec *);
OSErr FSpOpenDF(const FSSpec *, char, short *);
OSErr FSpCreate(const FSSpec *, OSType, OSType, short);
OSErr GetEOF(short, long *); OSErr SetFPos(short, short, long);
OSErr FSWrite(short, long *, const void *); OSErr FSClose(short);
OSErr FlushVol(ConstStr255Param, short);
#define kOnSystemDisk ((short)0x8000)
#define kExtensionFolderType 0x6578746eUL
#define kDontCreateFolder 0
OSErr FindFolder(short, OSType, Boolean, short *, long *);
typedef struct { OSType componentType, componentSubType, componentManufacturer;
                 unsigned long componentFlags, componentFlagsMask; } ComponentDescription;
typedef struct ComponentRecord *Component;
typedef void *ComponentRoutineUPP;
Component FindNextComponent(Component, ComponentDescription *);
Component RegisterComponent(ComponentDescription *, ComponentRoutineUPP, short, Handle, Handle, Handle);
typedef struct OpaqueCFragConnectionID *CFragConnectionID;
typedef unsigned char CFragSymbolClass;
#define kCFragGoesToEOF 0
OSErr GetDiskFragment(const FSSpec *, UInt32, UInt32, ConstStr63Param, UInt32,
                      CFragConnectionID *, Ptr *, Str255);
OSErr FindSymbol(CFragConnectionID, ConstStr255Param, Ptr *, CFragSymbolClass *);
OSErr CloseConnection(CFragConnectionID *);
OSErr EnterMovies(void);
Ptr NewPtrSysClear(Size); Ptr NewPtrSys(Size); void DisposePtr(Ptr);
Handle NewHandle(Size); void BlockMoveData(const void *, void *, Size);
void MaxApplZone(void); void MoreMasters(void);
typedef struct { unsigned short what; unsigned long message, when; short h, v; unsigned short modifiers; } EventRecord;
#define everyEvent 0xFFFF
Boolean WaitNextEvent(unsigned short, EventRecord *, unsigned long, void *);
typedef struct { void *thePort; } QDGlobals; extern QDGlobals qd;
void InitGraf(void *); void InitFonts(void); void InitWindows(void); void InitMenus(void);
void TEInit(void); void InitDialogs(void *);
typedef void *NMUPP;
#define nmType 8
typedef struct NMRec { void *qLink; short qType; short nmFlags; long nmPrivate; short nmReserved;
    short nmMark; Handle nmIcon; Handle nmSound; StringPtr nmStr; NMUPP nmResp; long nmRefCon; } NMRec;
typedef NMRec *NMRecPtr;
OSErr NMInstall(NMRecPtr);
#define gestaltQuickTimeVersion 0x7174696dUL
OSErr Gestalt(OSType selector, long *response);
/* Apple Event Manager (UI 3.4 spellings) -- added for the Quit handler */
typedef struct { unsigned long descriptorType; Handle dataHandle; } AEDesc;
typedef AEDesc AppleEvent;
typedef pascal OSErr (*AEEventHandlerProcPtr)(const AppleEvent *, AppleEvent *, long);
typedef AEEventHandlerProcPtr AEEventHandlerUPP;
#define NewAEEventHandlerUPP(p) ((AEEventHandlerUPP)(p))
#define kCoreEventClass     0x61657674UL
#define kAEOpenApplication  0x6f617070UL
#define kAEQuitApplication  0x71756974UL
#define kHighLevelEvent     23
OSErr AEInstallEventHandler(unsigned long, unsigned long, AEEventHandlerUPP, long, Boolean);
OSErr AEProcessAppleEvent(const EventRecord *);
unsigned long TickCount(void);
long  CountComponentInstances(Component);
OSErr UnregisterComponent(Component);
#endif
