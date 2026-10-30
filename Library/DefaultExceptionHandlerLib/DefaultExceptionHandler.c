/** @file
  Default exception handler, Exynos441x fork of ArmPkg's version.

  Differences from ArmPkg/Library/DefaultExceptionHandlerLib/Arm:

    - Wipes the framebuffer and resets the debug console cursor first, so the
      dump starts on a clean screen at the top-left instead of scrolling into
      whatever was printed before the fault.
    - Also asks ConOut to clear itself (best effort, guarded against a second
      fault inside the console) so the text console cursor is reset too.
    - Everything goes out through DEBUG() only: no SerialPortWrite(), no
      gST->ConOut->OutputString(), no Print().
    - No ASSERT(): it dead-loops straight away so the dump stays on screen and
      can be photographed.

  The dump is the interesting part: exception type, PC, CPSR decoded, the
  module the PC belongs to (PE/COFF path + RVA, which maps 1:1 onto the
  link map), all ARM registers, and the DFSR/DFAR/IFSR/IFAR fault registers
  decoded into a fault source.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/PeCoffGetEntryPointLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Guid/DebugImageInfoTable.h>

#include <Protocol/DebugSupport.h>
#include <Library/DefaultExceptionHandlerLib.h>

//
// Private entry points of FrameBufferSerialPortLib (no public header for
// them): wipe the panel and put the DEBUG cursor back at the top-left.
//
void
ResetFb (void);

void
FbConReset (void);

#define CPSR_STRING_SIZE  32

typedef struct {
  UINT32  BIT;
  CHAR8   Char;
} CPSR_CHAR;

STATIC CONST CPSR_CHAR  mCpsrChar[] = {
  { 31, 'n' },
  { 30, 'z' },
  { 29, 'c' },
  { 28, 'v' },

  { 9,  'e' },
  { 8,  'a' },
  { 7,  'i' },
  { 6,  'f' },
  { 5,  't' },
  { 0,  '?' }
};

STATIC CONST CHAR8  *mExceptionTypeString[] = {
  "Reset",
  "Undefined OpCode",
  "SVC",
  "Prefetch Abort",
  "Data Abort",
  "Undefined",
  "IRQ",
  "FIQ"
};

//
// Set on entry: a fault inside the console clear below must not recurse
// forever.  Second entry skips ConOut and goes straight to the raw dump.
//
STATIC BOOLEAN  mInExceptionHandler = FALSE;

CHAR8 *
GetImageName (
  IN  UINTN  FaultAddress,
  OUT UINTN  *ImageBase,
  OUT UINTN  *PeCoffSizeOfHeaders
  );

/**
  Convert the Current Program Status Register (CPSR) to a string. The string is
  a defacto standard in the ARM world.

  @param  Cpsr         ARM CPSR register value
  @param  ReturnStr    CPSR_STRING_SIZE byte string that contains string
                       version of the CPSR

**/
STATIC
VOID
CpsrString (
  IN  UINT32  Cpsr,
  OUT CHAR8   *ReturnStr
  )
{
  UINTN  Index;
  CHAR8  *Str;
  CHAR8  *ModeStr;

  Str = ReturnStr;

  for (Index = 0; mCpsrChar[Index].BIT != 0; Index++, Str++) {
    *Str = mCpsrChar[Index].Char;
    if ((Cpsr & (1 << mCpsrChar[Index].BIT)) != 0) {
      // Convert to upper case if bit is set
      *Str &= ~0x20;
    }
  }

  *Str++ = '_';
  *Str   = '\0';

  switch (Cpsr & 0x1f) {
    case 0x10:  ModeStr = "usr";
      break;
    case 0x11:  ModeStr = "fiq";
      break;
    case 0x12:  ModeStr = "irq";
      break;
    case 0x13:  ModeStr = "svc";
      break;
    case 0x16:  ModeStr = "mon";
      break;
    case 0x17:  ModeStr = "abt";
      break;
    case 0x1b:  ModeStr = "und";
      break;
    case 0x1f:  ModeStr = "sys";
      break;
    default:    ModeStr = "???";
      break;
  }

  AsciiStrCatS (Str, CPSR_STRING_SIZE - (Str - ReturnStr), ModeStr);
}

/**
  Decode a DFSR/IFSR status code into a human readable fault source.

  Bit10 is Status[4], Bit3:0 is Status[3:0].

  @param  Status  DFSR or IFSR fault status value

  @retval String describing the fault

**/
STATIC
CHAR8 *
FaultStatusToString (
  IN  UINT32  Status
  )
{
  switch (Status) {
    case 0x01: return "Alignment fault";
    case 0x02: return "Debug event fault";
    case 0x03: return "Access Flag fault on Section";
    case 0x04: return "Cache maintenance operation fault[2]";
    case 0x05: return "Translation fault on Section";
    case 0x06: return "Access Flag fault on Page";
    case 0x07: return "Translation fault on Page";
    case 0x08: return "Precise External Abort";
    case 0x09: return "Domain fault on Section";
    case 0x0b: return "Domain fault on Page";
    case 0x0c: return "External abort on translation, first level";
    case 0x0d: return "Permission fault on Section";
    case 0x0e: return "External abort on translation, second level";
    case 0x0f: return "Permission fault on Page";
    case 0x16: return "Imprecise External Abort";
    default:   return "No function";
  }
}

/**
  This is the default action to take on an unexpected exception.

  Since this is exception context don't do anything crazy like try to allocate
  memory.

  @param  ExceptionType    Type of the exception
  @param  SystemContext    Register state at the time of the Exception

**/
VOID
DefaultExceptionHandler (
  IN     EFI_EXCEPTION_TYPE  ExceptionType,
  IN OUT EFI_SYSTEM_CONTEXT  SystemContext
  )
{
  CONST CHAR8  *TypeString;
  CHAR8        CpsrStr[CPSR_STRING_SIZE];
  UINTN        ImageBase;
  UINTN        PeCoffSizeOfHeader;
  UINTN        Offset;
  CHAR8        *Pdb;
  UINT32       DfsrStatus;
  UINT32       IfsrStatus;
  BOOLEAN      DfsrWrite;

  //
  // 1. Clear the console (best effort) and the framebuffer, and put the DEBUG
  //    cursor back at the top-left so the dump gets the whole clean screen.
  //
  //    Note: this also wipes a preceding "*** ASSERT ***" line if the
  //    exception came from CpuBreakpoint().  The stack dump below identifies
  //    that case -- walk the frames back into the module named above and map
  //    them exactly like the RVA line.
  //
  if (!mInExceptionHandler) {
    mInExceptionHandler = TRUE;
    if ((gST != NULL) && (gST->ConOut != NULL)) {
      gST->ConOut->ClearScreen (gST->ConOut);
    }
  }

  ResetFb ();
  FbConReset ();

  TypeString = (ExceptionType < ARRAY_SIZE (mExceptionTypeString)) ?
               mExceptionTypeString[ExceptionType] :
               "Unknown";

  //
  // 2. Everything below is DEBUG() only -- it reaches the panel through
  //    FrameBufferSerialPortLib and never touches gST->ConOut or Print().
  //
  DEBUG ((DEBUG_ERROR, "\n===== %a EXCEPTION =====\n", TypeString));
  DEBUG ((DEBUG_ERROR, "PC  0x%08x   CPSR 0x%08x\n",
          SystemContext.SystemContextArm->PC,
          SystemContext.SystemContextArm->CPSR
          ));

  CpsrString (SystemContext.SystemContextArm->CPSR, CpsrStr);
  DEBUG ((DEBUG_ERROR, "CPSR bits %a\n", CpsrStr));

  //
  // Which module is the PC in?  The RVA maps 1:1 onto the link map/objdump
  // addresses of that module's ELF, so this identifies the faulting code.
  //
  Pdb = GetImageName (
          SystemContext.SystemContextArm->PC,
          &ImageBase,
          &PeCoffSizeOfHeader
          );
  if (Pdb != NULL) {
    Offset = SystemContext.SystemContextArm->PC - ImageBase;
    DEBUG ((DEBUG_ERROR, "in %a\n", Pdb));
    DEBUG ((DEBUG_ERROR, "    base 0x%08x  RVA 0x%08x  ELF/COFF 0x%x\n",
            ImageBase, Offset, Offset - PeCoffSizeOfHeader));
  }

  DEBUG ((DEBUG_ERROR, "\n  R0 0x%08x   R1 0x%08x   R2 0x%08x   R3 0x%08x\n",
          SystemContext.SystemContextArm->R0, SystemContext.SystemContextArm->R1,
          SystemContext.SystemContextArm->R2, SystemContext.SystemContextArm->R3));
  DEBUG ((DEBUG_ERROR, "  R4 0x%08x   R5 0x%08x   R6 0x%08x   R7 0x%08x\n",
          SystemContext.SystemContextArm->R4, SystemContext.SystemContextArm->R5,
          SystemContext.SystemContextArm->R6, SystemContext.SystemContextArm->R7));
  DEBUG ((DEBUG_ERROR, "  R8 0x%08x   R9 0x%08x  R10 0x%08x  R11 0x%08x\n",
          SystemContext.SystemContextArm->R8, SystemContext.SystemContextArm->R9,
          SystemContext.SystemContextArm->R10, SystemContext.SystemContextArm->R11));
  DEBUG ((DEBUG_ERROR, " R12 0x%08x   SP 0x%08x   LR 0x%08x   PC 0x%08x\n",
          SystemContext.SystemContextArm->R12, SystemContext.SystemContextArm->SP,
          SystemContext.SystemContextArm->LR, SystemContext.SystemContextArm->PC));
  DEBUG ((DEBUG_ERROR, "DFSR 0x%08x DFAR 0x%08x IFSR 0x%08x IFAR 0x%08x\n",
          SystemContext.SystemContextArm->DFSR, SystemContext.SystemContextArm->DFAR,
          SystemContext.SystemContextArm->IFSR, SystemContext.SystemContextArm->IFAR));

  //
  // The user stack: return addresses of the faulting call chain.  Values in
  // the 0x7Bxxxxxx range are code inside the module named above, so
  // "PC - ImageBase" gives the same 1:1 objdump offsets as the RVA line.
  //
  {
    UINT32  *Sp;
    UINTN   Index;

    Sp = (UINT32 *)(UINTN)SystemContext.SystemContextArm->SP;
    DEBUG ((DEBUG_ERROR, "stack @ SP 0x%08x:\n", SystemContext.SystemContextArm->SP));
    for (Index = 0; Index < 24; Index++) {
      DEBUG ((DEBUG_ERROR, "%a  %08x %08x %08x %08x\n",
              (Index % 4 == 0) ? (Index == 0 ? "  +" : "   ") : "   ",
              Sp[Index * 4 + 0], Sp[Index * 4 + 1],
              Sp[Index * 4 + 2], Sp[Index * 4 + 3]));
    }
  }

  //
  // Decode the fault: what kind, and read or write to which address.
  //
  DfsrStatus = (SystemContext.SystemContextArm->DFSR & 0xf) |
               ((SystemContext.SystemContextArm->DFSR >> 6) & 0x10);
  DfsrWrite  = (SystemContext.SystemContextArm->DFSR & BIT11) != 0;
  if (DfsrStatus != 0x00) {
    DEBUG ((DEBUG_ERROR, " %a: %a 0x%08x\n",
            FaultStatusToString (DfsrStatus),
            DfsrWrite ? "write to" : "read from",
            SystemContext.SystemContextArm->DFAR));
  }

  IfsrStatus = (SystemContext.SystemContextArm->IFSR & 0xf) |
               ((SystemContext.SystemContextArm->IFSR >> 6) & 0x10);
  if (IfsrStatus != 0) {
    DEBUG ((DEBUG_ERROR, " Instruction %a at 0x%08x\n",
            FaultStatusToString (SystemContext.SystemContextArm->IFSR & 0xf),
            SystemContext.SystemContextArm->IFAR));
  }

  DEBUG ((DEBUG_ERROR, "===== end of %a exception, hanging =====\n\n", TypeString));

  //
  // 3. Hold the screen.  No ASSERT() -- just stop here so the dump stays put.
  //
  for(;;);
  CpuDeadLoop ();

  //
  // Clear the error registers we have already displayed, in case some one
  // wants to keep going (a debugger can step past the dead loop).
  //
  SystemContext.SystemContextArm->DFSR = 0;
  SystemContext.SystemContextArm->IFSR = 0;
}
