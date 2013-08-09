/*
 * COPYRIGHT:       GPL - See COPYING in the top level directory
 * PROJECT:         ReactOS Virtual DOS Machine
 * FILE:            vga.c
 * PURPOSE:         VGA hardware emulation
 * PROGRAMMERS:     Aleksandar Andrejevic <theflash AT sdf DOT lonestar DOT org>
 */

/* INCLUDES *******************************************************************/

#define NDEBUG

#include "vga.h"
#include "bios.h"

/* PRIVATE VARIABLES **********************************************************/

static CONST DWORD MemoryBase[] = { 0xA0000, 0xA0000, 0xB0000, 0xB8000 };
static CONST DWORD MemoryLimit[] = { 0xAFFFF, 0xAFFFF, 0xB7FFF, 0xBFFFF };

static BYTE VgaMemory[VGA_NUM_BANKS * VGA_BANK_SIZE];
static BYTE VgaMiscRegister;
static BYTE VgaSeqIndex = VGA_SEQ_RESET_REG;
static BYTE VgaSeqRegisters[VGA_SEQ_MAX_REG];
static BYTE VgaGcIndex = VGA_GC_RESET_REG;
static BYTE VgaGcRegisters[VGA_GC_MAX_REG];
static BYTE VgaCrtcIndex = VGA_CRTC_HORZ_TOTAL_REG;
static BYTE VgaCrtcRegisters[VGA_CRTC_MAX_REG];
static BYTE VgaAcIndex = VGA_AC_PAL_0_REG;
static BOOLEAN VgaAcLatch = FALSE;
static BYTE VgaAcRegisters[VGA_AC_MAX_REG];
static BYTE VgaDacIndex = 0;
static BOOLEAN VgaDacReadWrite = FALSE;
static BYTE VgaDacRegisters[VGA_PALETTE_SIZE];
static BOOLEAN InVerticalRetrace = FALSE;
static BOOLEAN InHorizontalRetrace = FALSE;
static HANDLE TextConsoleBuffer = NULL;
static HANDLE GraphicsConsoleBuffer = NULL;
static LPVOID ConsoleFramebuffer = NULL;
static HANDLE ConsoleMutex = NULL;
static BOOLEAN NeedsUpdate = FALSE;
static BOOLEAN ModeChanged = TRUE;
static BOOLEAN CursorMoved = FALSE;
static BOOLEAN TextMode = TRUE;
static SMALL_RECT UpdateRectangle = { 0, 0, 0, 0 };

/* PRIVATE FUNCTIONS **********************************************************/

static inline INT VgaGetAddressSize(VOID)
{
    if (VgaCrtcRegisters[VGA_CRTC_UNDERLINE_REG] & VGA_CRTC_UNDERLINE_DWORD)
    {
        /* Double-word addressing */
        return 4;
    }
    
    if (VgaCrtcRegisters[VGA_CRTC_MODE_CONTROL_REG] & VGA_CRTC_MODE_CONTROL_BYTE)
    {
        /* Byte addressing */
        return 1;
    }

    /* Word addressing */
    return 2;
}

static inline DWORD VgaTranslateReadAddress(DWORD Address)
{
    DWORD Offset = Address - VgaGetVideoBaseAddress();
    BYTE Plane;

    /* Check for chain-4 and odd-even mode */
    if (VgaSeqRegisters[VGA_SEQ_MEM_REG] & VGA_SEQ_MEM_C4)
    {
        /* The lowest two bits are the plane number */
        Plane = Offset & 3;
        Offset >>= 2;
    }
    else if (VgaGcRegisters[VGA_GC_MODE_REG] & VGA_GC_MODE_OE)
    {
        /* The LSB is the plane number */
        Plane = Offset & 1;
        Offset >>= 1;
    }
    else
    {
        /* Use the read mode */
        Plane = VgaGcRegisters[VGA_GC_READ_MAP_SEL_REG] & 0x03;
    }

    /* Multiply the offset by the address size */
    Offset *= VgaGetAddressSize();
    
    return Offset + Plane * VGA_BANK_SIZE;
}

static inline DWORD VgaTranslateWriteAddress(DWORD Address)
{
    DWORD Offset = Address - VgaGetVideoBaseAddress();

    /* Check for chain-4 and odd-even mode */
    if (VgaSeqRegisters[VGA_SEQ_MEM_REG] & VGA_SEQ_MEM_C4)
    {
        /* Shift the offset to the right by 2 */
        Offset >>= 2;
    }
    else if (VgaGcRegisters[VGA_GC_MODE_REG] & VGA_GC_MODE_OE)
    {
        /* Shift the offset to the right by 1 */
        Offset >>= 1;
    }

    /* Multiply the offset by the address size */
    Offset *= VgaGetAddressSize();

    /* Return the offset on plane 0 */
    return Offset;
}

static inline VOID VgaMarkForUpdate(SHORT Row, SHORT Column)
{
    DPRINT("VgaMarkForUpdate: Row %d, Column %d\n", Row, Column);

    /* Check if this is the first time the rectangle is updated */
    if (!NeedsUpdate)
    {
        UpdateRectangle.Left = UpdateRectangle.Top = SHRT_MAX;
        UpdateRectangle.Right = UpdateRectangle.Bottom = SHRT_MIN;
    }

    /* Expand the rectangle to include the point */
    UpdateRectangle.Left = min(UpdateRectangle.Left, Column);
    UpdateRectangle.Right = max(UpdateRectangle.Right, Column);
    UpdateRectangle.Top = min(UpdateRectangle.Top, Row);
    UpdateRectangle.Bottom = max(UpdateRectangle.Bottom, Row);

    /* Set the update request flag */
    NeedsUpdate = TRUE;
}

static VOID VgaWriteSequencer(BYTE Data)
{
    ASSERT(VgaSeqIndex < VGA_SEQ_MAX_REG);

    /* Save the value */
    VgaSeqRegisters[VgaSeqIndex] = Data;
}

static VOID VgaWriteGc(BYTE Data)
{
    ASSERT(VgaGcIndex < VGA_GC_MAX_REG);

    /* Save the value */
    VgaGcRegisters[VgaGcIndex] = Data;

    /* Check the index */
    switch (VgaGcIndex)
    {
        case VGA_GC_MISC_REG:
        {
            /* The GC misc register decides if it's text or graphics mode */
            ModeChanged = TRUE;

            break;
        }
    }
}

static VOID VgaWriteCrtc(BYTE Data)
{
    ASSERT(VgaGcIndex < VGA_CRTC_MAX_REG);

    /* Save the value */
    VgaCrtcRegisters[VgaCrtcIndex] = Data;

    /* Check the index */
    switch (VgaCrtcIndex)
    {
        case VGA_CRTC_END_HORZ_DISP_REG:
        case VGA_CRTC_VERT_DISP_END_REG:
        case VGA_CRTC_OVERFLOW_REG:
        {
            /* The video mode has changed */
            ModeChanged = TRUE;

            break;
        }

        case VGA_CRTC_CURSOR_LOC_LOW_REG:
        case VGA_CRTC_CURSOR_LOC_HIGH_REG:
        case VGA_CRTC_CURSOR_START_REG:
        case VGA_CRTC_CURSOR_END_REG:
        {
            /* Set the cursor moved flag */
            CursorMoved = TRUE;

            break;
        }
    }
}

static VOID VgaWriteDac(BYTE Data)
{
    /* Set the value */
    VgaDacRegisters[VgaDacIndex++] = Data;
    VgaDacIndex %= VGA_PALETTE_SIZE;

    // TODO: Change the palette!
}

static VOID VgaWriteAc(BYTE Data)
{
    ASSERT(VgaAcIndex < VGA_AC_MAX_REG);

    /* Save the value */
    VgaAcRegisters[VgaAcIndex] = Data;
}

static BOOL VgaEnterGraphicsMode(PCOORD Resolution)
{
    DWORD i;
    CONSOLE_GRAPHICS_BUFFER_INFO GraphicsBufferInfo;
    BYTE BitmapInfoBuffer[VGA_BITMAP_INFO_SIZE];
    LPBITMAPINFO BitmapInfo = (LPBITMAPINFO)BitmapInfoBuffer;
    LPWORD PaletteIndex = (LPWORD)(BitmapInfoBuffer + sizeof(BITMAPINFOHEADER));

    /* Fill the bitmap info header */
    ZeroMemory(&BitmapInfo->bmiHeader, sizeof(BITMAPINFOHEADER));
    BitmapInfo->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    BitmapInfo->bmiHeader.biWidth = Resolution->X;
    BitmapInfo->bmiHeader.biHeight = Resolution->Y;
    BitmapInfo->bmiHeader.biBitCount = 8;
    BitmapInfo->bmiHeader.biPlanes = 1;
    BitmapInfo->bmiHeader.biCompression = BI_RGB;
    BitmapInfo->bmiHeader.biSizeImage = Resolution->X * Resolution->Y;

    /* Fill the palette data */
    for (i = 0; i < (VGA_PALETTE_SIZE / 3); i++) PaletteIndex[i] = (WORD)i;

    /* Fill the console graphics buffer info */
    GraphicsBufferInfo.dwBitMapInfoLength = VGA_BITMAP_INFO_SIZE;
    GraphicsBufferInfo.lpBitMapInfo = BitmapInfo;
    GraphicsBufferInfo.dwUsage = DIB_PAL_COLORS;

    /* Create the buffer */
    GraphicsConsoleBuffer = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE,
                                                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                      NULL,
                                                      CONSOLE_GRAPHICS_BUFFER,
                                                      &GraphicsBufferInfo);
    if (GraphicsConsoleBuffer == INVALID_HANDLE_VALUE) return FALSE;

    /* Save the framebuffer address and mutex */
    ConsoleFramebuffer = GraphicsBufferInfo.lpBitMap;
    ConsoleMutex = GraphicsBufferInfo.hMutex;

    /* Clear the framebuffer */
    ZeroMemory(ConsoleFramebuffer, BitmapInfo->bmiHeader.biSizeImage);

    /* Set the active buffer */
    SetConsoleActiveScreenBuffer(GraphicsConsoleBuffer);

    return TRUE;
}

static VOID VgaLeaveGraphicsMode(VOID)
{
    /* Release the console framebuffer mutex if needed */
    ReleaseMutex(ConsoleMutex);

    /* Switch back to the text buffer */
    SetConsoleActiveScreenBuffer(TextConsoleBuffer);

    /* Cleanup the video data */
    CloseHandle(ConsoleMutex);
    ConsoleMutex = NULL;
    CloseHandle(GraphicsConsoleBuffer);
    GraphicsConsoleBuffer = NULL;
}

static BOOL VgaEnterTextMode(PCOORD Resolution)
{
    /* Resize the console */
    SetConsoleScreenBufferSize(TextConsoleBuffer, *Resolution);

    /* Allocate a framebuffer */
    ConsoleFramebuffer = HeapAlloc(GetProcessHeap(),
                                   HEAP_ZERO_MEMORY,
                                   Resolution->X * Resolution->Y
                                   * sizeof(CHAR_INFO));
    if (ConsoleFramebuffer == NULL)
    {
        DisplayMessage(L"An unexpected error occurred!\n");
        VdmRunning = FALSE;
        return FALSE;
    }

    return TRUE;
}

static VOID VgaLeaveTextMode(VOID)
{
    /* Free the old framebuffer */
    HeapFree(GetProcessHeap(), 0, ConsoleFramebuffer);
    ConsoleFramebuffer = NULL;
}

static VOID VgaUpdateMode(VOID)
{
    COORD Resolution = VgaGetDisplayResolution();

    if (!TextMode)
    {
        /* Leave the current graphics mode */
        VgaLeaveGraphicsMode();
    }
    else
    {
        /* Leave the current text mode */
        VgaLeaveTextMode();
    }

    /* Check if the new mode is alphanumeric */
    if (!(VgaGcRegisters[VGA_GC_MISC_REG] & VGA_GC_MISC_NOALPHA))
    {
        /* Enter new text mode */
        if (!VgaEnterTextMode(&Resolution)) return;

        /* Set the text mode flag */
        TextMode = TRUE;
    }
    else
    {
        /* Enter 8-bit graphics mode */
        if (!VgaEnterGraphicsMode(&Resolution)) return;

        /* Clear the text mode flag */
        TextMode = FALSE;
    }

    /* Perform a full update */
    NeedsUpdate = TRUE;
    UpdateRectangle.Left = 0;
    UpdateRectangle.Top = 0;
    UpdateRectangle.Right = Resolution.X;
    UpdateRectangle.Bottom = Resolution.Y;
}

static VOID VgaUpdateFramebuffer(VOID)
{
    INT i, j, k;
    COORD Resolution = VgaGetDisplayResolution();
    INT AddressSize = VgaGetAddressSize();
    DWORD Address = (VgaCrtcRegisters[VGA_CRTC_START_ADDR_HIGH_REG] << 8)
                    + VgaCrtcRegisters[VGA_CRTC_START_ADDR_LOW_REG];
    DWORD ScanlineSize = (DWORD)VgaCrtcRegisters[VGA_CRTC_OFFSET_REG] * 2;

    /* Check if this is text mode or graphics mode */
    if (VgaGcRegisters[VGA_GC_MISC_REG] & VGA_GC_MISC_NOALPHA)
    {
        /* Graphics mode */
        PBYTE GraphicsBuffer = (PBYTE)ConsoleFramebuffer;

        /*
         * Synchronize access to the graphics framebuffer
         * with the console framebuffer mutex.
         */
        WaitForSingleObject(ConsoleMutex, INFINITE);

        /* Loop through the scanlines */
        for (i = 0; i < Resolution.Y; i++)
        {
            /* Loop through the pixels */
            for (j = 0; j < Resolution.X; j++)
            {
                BYTE PixelData = 0;

                /* Check the shifting mode */
                if (VgaGcRegisters[VGA_GC_MODE_REG] & VGA_GC_MODE_SHIFT256)
                {
                    /* 4 bits shifted from each plane */

                    /* Check if this is 16 or 256 color mode */
                    if (VgaAcRegisters[VGA_AC_CONTROL_REG] & VGA_AC_CONTROL_8BIT)
                    {
                        /* One byte per pixel */
                        PixelData = VgaMemory[(j % VGA_NUM_BANKS) * VGA_BANK_SIZE
                                              + (Address + (j / VGA_NUM_BANKS))
                                              * AddressSize];
                    }
                    else
                    {
                        /* 4-bits per pixel */

                        PixelData = VgaMemory[(j % VGA_NUM_BANKS) * VGA_BANK_SIZE
                                              + (Address + (j / (VGA_NUM_BANKS * 2)))
                                              * AddressSize];

                        /* Check if we should use the highest 4 bits or lowest 4 */
                        if (((j / VGA_NUM_BANKS) % 2) == 0)
                        {
                            /* Highest 4 */
                            PixelData >>= 4;
                        }
                        else
                        {
                            /* Lowest 4 */
                            PixelData &= 0x0F;
                        }
                    }
                }
                else if (VgaGcRegisters[VGA_GC_MODE_REG] & VGA_GC_MODE_SHIFTREG)
                {
                    /*
                     * 2 bits shifted from plane 0 and 2 for the first 4 pixels,
                     * then 2 bits shifted from plane 1 and 3 for the next 4
                     */

                    // TODO: NOT IMPLEMENTED!
                    DPRINT1("Interleaved shift mode is not implemented!\n");
                }
                else
                {
                    /* 1 bit shifted from each plane */

                    /* Check if this is 16 or 256 color mode */
                    if (VgaAcRegisters[VGA_AC_CONTROL_REG] & VGA_AC_CONTROL_8BIT)
                    {
                        /* 8 bits per pixel, 2 on each plane */

                        for (k = 0; k < VGA_NUM_BANKS; k++)
                        {
                            /* The data is on plane k, 4 pixels per byte */
                            BYTE PlaneData = VgaMemory[k * VGA_BANK_SIZE
                                                       + (Address + (j / 4)) * AddressSize];

                            /* The mask of the first bit in the pair */
                            BYTE BitMask = 1 << (((3 - (j % 4)) * 2) + 1);

                            /* Bits 0, 1, 2 and 3 come from the first bit of the pair */
                            if (PlaneData & BitMask) PixelData |= 1 << k;

                            /* Bits 4, 5, 6 and 7 come from the second bit of the pair */
                            if (PlaneData & (BitMask >> 1)) PixelData |= 1 << (k + 4);
                        }
                    }
                    else
                    {
                        /* 4 bits per pixel, 1 on each plane */

                        for (k = 0; k < VGA_NUM_BANKS; k++)
                        {
                            BYTE PlaneData = VgaMemory[k * VGA_BANK_SIZE
                                                       + (Address + (j / 8)) * AddressSize];

                            /* If the bit on that plane is set, set it */
                            if (PlaneData & (1 << (7 - (j % 8)))) PixelData |= 1 << k;
                        }
                    }
                }

                /* Now check if the resulting pixel data has changed */
                if (GraphicsBuffer[i * Resolution.X + j] != PixelData)
                {
                    /* Yes, write the new value */
                    GraphicsBuffer[i * Resolution.X + j] = PixelData;

                    /* Mark the specified pixel as changed */
                    VgaMarkForUpdate(i, j);
                }
            }

            /* Move to the next scanline */
            Address += ScanlineSize;
        }

        /*
         * Release the console framebuffer mutex
         * so that we allow for repainting.
         */
        ReleaseMutex(ConsoleMutex);
    }
    else
    {
        /* Text mode */
        PCHAR_INFO CharBuffer = (PCHAR_INFO)ConsoleFramebuffer;

        /* Loop through the scanlines */
        for (i = 0; i < Resolution.Y; i++)
        {
            /* Loop through the characters */
            for (j = 0; j < Resolution.X; j++)
            {
                DWORD CurrentAddr = LOWORD((Address + j) * AddressSize);
                CHAR_INFO CharInfo;

                /* Plane 0 holds the character itself */
                CharInfo.Char.AsciiChar = VgaMemory[CurrentAddr];

                /* Plane 1 holds the attribute */
                CharInfo.Attributes = VgaMemory[CurrentAddr + VGA_BANK_SIZE];

                /* Now check if the resulting character data has changed */
                if ((CharBuffer[i * Resolution.X + j].Char.AsciiChar != CharInfo.Char.AsciiChar)
                    || (CharBuffer[i * Resolution.X + j].Attributes != CharInfo.Attributes))
                {
                    /* Yes, write the new value */
                    CharBuffer[i * Resolution.X + j] = CharInfo;

                    /* Mark the specified pixel as changed */
                    VgaMarkForUpdate(i, j);
                }
            }

            /* Move to the next scanline */
            Address += ScanlineSize;
        }
    }
}

static VOID VgaUpdateTextCursor(VOID)
{
    COORD Position;
    CONSOLE_CURSOR_INFO CursorInfo;
    BYTE CursorStart = VgaCrtcRegisters[VGA_CRTC_CURSOR_START_REG] & 0x3F;
    BYTE CursorEnd = VgaCrtcRegisters[VGA_CRTC_CURSOR_END_REG] & 0x1F;
    DWORD ScanlineSize = (DWORD)VgaCrtcRegisters[VGA_CRTC_OFFSET_REG] * 2;
    BYTE TextSize = 1 + (VgaCrtcRegisters[VGA_CRTC_MAX_SCAN_LINE_REG] & 0x1F);
    WORD Location = MAKEWORD(VgaCrtcRegisters[VGA_CRTC_CURSOR_LOC_LOW_REG],
                             VgaCrtcRegisters[VGA_CRTC_CURSOR_LOC_HIGH_REG]);

    if (CursorStart < CursorEnd)
    {
        /* Visible cursor */
        CursorInfo.bVisible = TRUE;
        CursorInfo.dwSize = (100 * (CursorEnd - CursorStart)) / TextSize;
    }
    else
    {
        /* No cursor */
        CursorInfo.bVisible = FALSE;
        CursorInfo.dwSize = 0;
    }

    /* Add the cursor skew to the location */
    Location += (VgaCrtcRegisters[VGA_CRTC_CURSOR_END_REG] >> 5) & 3;

    /* Find the coordinates of the new position */
    Position.X = (WORD)(Location % ScanlineSize);
    Position.Y = (WORD)(Location / ScanlineSize);

    /* Update the physical cursor */
    SetConsoleCursorInfo(TextConsoleBuffer, &CursorInfo);
    SetConsoleCursorPosition(TextConsoleBuffer, Position);
}

/* PUBLIC FUNCTIONS ***********************************************************/

DWORD VgaGetVideoBaseAddress(VOID)
{
    return MemoryBase[(VgaGcRegisters[VGA_GC_MISC_REG] >> 2) & 0x03];
}

DWORD VgaGetVideoLimitAddress(VOID)
{
    return MemoryLimit[(VgaGcRegisters[VGA_GC_MISC_REG] >> 2) & 0x03];
}

COORD VgaGetDisplayResolution(VOID)
{
    COORD Resolution;
    BYTE MaximumScanLine = 1 + (VgaCrtcRegisters[VGA_CRTC_MAX_SCAN_LINE_REG] & 0x1F);

    /* The low 8 bits are in the display registers */
    Resolution.X = VgaCrtcRegisters[VGA_CRTC_END_HORZ_DISP_REG];
    Resolution.Y = VgaCrtcRegisters[VGA_CRTC_VERT_DISP_END_REG];

    /* Set the top bits from the overflow register */
    if (VgaCrtcRegisters[VGA_CRTC_OVERFLOW_REG] & VGA_CRTC_OVERFLOW_VDE8)
    {
        Resolution.Y |= 1 << 8;
    }
    if (VgaCrtcRegisters[VGA_CRTC_OVERFLOW_REG] & VGA_CRTC_OVERFLOW_VDE9)
    {
        Resolution.Y |= 1 << 9;
    }

    /* Increase the values by 1 */
    Resolution.X++;
    Resolution.Y++;

    if (VgaGcRegisters[VGA_GC_MISC_REG] & VGA_GC_MISC_NOALPHA)
    {
        /* Multiply the horizontal resolution by the 9/8 dot mode */
        Resolution.X *= (VgaSeqRegisters[VGA_SEQ_CLOCK_REG] & VGA_SEQ_CLOCK_98DM)
                        ? 8 : 9;

        /* The horizontal resolution is halved in 8-bit mode */
        if (VgaAcRegisters[VGA_AC_CONTROL_REG] & VGA_AC_CONTROL_8BIT) Resolution.X /= 2;
    }

    /* Divide the vertical resolution by the maximum scan line (== font size in text mode) */
    Resolution.Y /= MaximumScanLine;

    /* Return the resolution */
    return Resolution;
}

VOID VgaRefreshDisplay(VOID)
{
    COORD Resolution = VgaGetDisplayResolution();

    DPRINT("VgaRefreshDisplay\n");

    if (ModeChanged)
    {
        /* Change the display mode */
        VgaUpdateMode();

        /* Reset the mode change flag */
        ModeChanged = FALSE;
    }

    if (CursorMoved)
    {
        /* Change the text cursor location */
        VgaUpdateTextCursor();

        /* Reset the cursor move flag */
        CursorMoved = FALSE;
    }

    /* Update the contents of the framebuffer */
    VgaUpdateFramebuffer();

    /* Set the vertical retrace flag */
    InVerticalRetrace = TRUE;

    /* Ignore if there's nothing to update */
    if (!NeedsUpdate) return;

    DPRINT("Updating screen rectangle (%d, %d, %d, %d)\n",
           UpdateRectangle.Left,
           UpdateRectangle.Top,
           UpdateRectangle.Right,
           UpdateRectangle.Bottom);

    /* Check if this is text mode or graphics mode */
    if (VgaGcRegisters[VGA_GC_MISC_REG] & VGA_GC_MISC_NOALPHA)
    {
        /* Graphics mode */

        /* Redraw the screen */
        InvalidateConsoleDIBits(GraphicsConsoleBuffer, &UpdateRectangle);
    }
    else
    {
        /* Text mode */
        COORD Origin = { UpdateRectangle.Left, UpdateRectangle.Top };

        /* Write the data to the console */
        WriteConsoleOutputA(TextConsoleBuffer,
                            (PCHAR_INFO)ConsoleFramebuffer,
                            Resolution,
                            Origin,
                            &UpdateRectangle);

    }

    /* Clear the update flag */
    NeedsUpdate = FALSE;
}

VOID VgaHorizontalRetrace(VOID)
{
    /* Set the flag */
    InHorizontalRetrace = TRUE;
}

VOID VgaReadMemory(DWORD Address, LPBYTE Buffer, DWORD Size)
{
    DWORD i;
    DWORD VideoAddress;

    DPRINT("VgaReadMemory: Address 0x%08X, Size %lu\n",
           Address,
           Size);

    /* Ignore if video RAM access is disabled */
    if (!(VgaMiscRegister & VGA_MISC_RAM_ENABLED)) return;

    /* Loop through each byte */
    for (i = 0; i < Size; i++)
    {
        VideoAddress = VgaTranslateReadAddress(Address + i);

        /* Copy the value to the buffer */
        Buffer[i] = VgaMemory[VideoAddress];
    }
}

VOID VgaWriteMemory(DWORD Address, LPBYTE Buffer, DWORD Size)
{
    DWORD i, j;
    DWORD VideoAddress;

    DPRINT("VgaWriteMemory: Address 0x%08X, Size %lu\n",
           Address,
           Size);

    /* Ignore if video RAM access is disabled */
    if (!(VgaMiscRegister & VGA_MISC_RAM_ENABLED)) return;

    /* Also ignore if write access to all planes is disabled */
    if ((VgaSeqRegisters[VGA_SEQ_MASK_REG] & 0x0F) == 0x00) return;

    /* Loop through each byte */
    for (i = 0; i < Size; i++)
    {
        VideoAddress = VgaTranslateWriteAddress(Address + i);

        for (j = 0; j < VGA_NUM_BANKS; j++)
        {
            /* Make sure the page is writeable */
            if (!(VgaSeqRegisters[VGA_SEQ_MASK_REG] & (1 << j))) continue;

            /* Check if this is chain-4 mode */
            if (VgaSeqRegisters[VGA_SEQ_MEM_REG] & VGA_SEQ_MEM_C4)
            {
                if (((Address + i) & 3) != j)
                {
                    /* This plane will not be accessed */
                    continue;
                }
            }

            /* Check if this is odd-even mode */
            if (VgaGcRegisters[VGA_GC_MODE_REG] & VGA_GC_MODE_OE)
            {
                if (((Address + i) & 1) != (j & 1))
                {
                    /* This plane will not be accessed */
                    continue;
                }
            }

            /* Copy the value to the VGA memory */
            VgaMemory[VideoAddress + j * VGA_BANK_SIZE] = Buffer[i];
        }
    }
}

BYTE VgaReadPort(WORD Port)
{
    DPRINT("VgaReadPort: Port 0x%04X\n", Port);

    switch (Port)
    {
        case VGA_AC_INDEX:
        {
            return VgaAcIndex;
        }

        case VGA_AC_READ:
        {
            return VgaAcRegisters[VgaAcIndex];
        }

        case VGA_SEQ_INDEX:
        {
            return VgaSeqIndex;
        }
        
        case VGA_SEQ_DATA:
        {
            return VgaSeqRegisters[VgaSeqIndex];
        }

        case VGA_DAC_READ_INDEX:
        {
            /* This returns the read/write state */
            return VgaDacReadWrite ? 0 : 3;
        }

        case VGA_DAC_WRITE_INDEX:
        {
            return VgaDacIndex;
        }

        case VGA_DAC_DATA:
        {
            /* Ignore reads in write mode */
            if (!VgaDacReadWrite)
            {
                BYTE Data = VgaDacRegisters[VgaDacIndex++];
                VgaDacIndex %= VGA_PALETTE_SIZE;
                return Data;
            }

            break;
        }

        case VGA_MISC_READ:
        {
            return VgaMiscRegister;
        }

        case VGA_CRTC_INDEX:
        {
            return VgaCrtcIndex;
        }

        case VGA_CRTC_DATA:
        {
            return VgaCrtcRegisters[VgaCrtcIndex];
        }

        case VGA_GC_INDEX:
        {
            return VgaGcIndex;
        }

        case VGA_GC_DATA:
        {
            return VgaGcRegisters[VgaGcIndex];
        }

        case VGA_STAT_MONO:
        case VGA_STAT_COLOR:
        {
            BYTE Result = 0;

            /* Reset the AC latch */
            VgaAcLatch = FALSE;

            /* Set a flag if there is a vertical or horizontal retrace */
            if (InVerticalRetrace || InHorizontalRetrace) Result |= VGA_STAT_DD;

            /* Set an additional flag if there was a vertical retrace */
            if (InVerticalRetrace) Result |= VGA_STAT_VRETRACE;

            /* Clear the flags */
            InHorizontalRetrace = InVerticalRetrace = FALSE;

            return Result;
        }
    }
    
    return 0;
}

VOID VgaWritePort(WORD Port, BYTE Data)
{
    DPRINT("VgaWritePort: Port 0x%04X, Data 0x%02X\n", Port, Data);

    switch (Port)
    {
        case VGA_AC_INDEX:
        {
            if (!VgaAcLatch)
            {
                /* Change the index */
                if (Data < VGA_AC_MAX_REG) VgaAcIndex = Data;
            }
            else
            {
                /* Write the data */
                VgaWriteAc(Data);
            }

            /* Toggle the latch */
            VgaAcLatch = !VgaAcLatch;

            break;
        }

        case VGA_SEQ_INDEX:
        {
            /* Set the sequencer index register */
            if (Data < VGA_SEQ_MAX_REG) VgaSeqIndex = Data;
            
            break;
        }

        case VGA_SEQ_DATA:
        {
            /* Call the sequencer function */
            VgaWriteSequencer(Data);
            
            break;
        }

        case VGA_DAC_READ_INDEX:
        {
            VgaDacReadWrite = FALSE;
            VgaDacIndex = Data % VGA_PALETTE_SIZE;

            break;
        }

        case VGA_DAC_WRITE_INDEX:
        {
            VgaDacReadWrite = TRUE;
            VgaDacIndex = Data % VGA_PALETTE_SIZE;

            break;
        }

        case VGA_DAC_DATA:
        {
            /* Ignore writes in read mode */
            if (VgaDacReadWrite) VgaWriteDac(Data & 0x3F);

            break;
        }

        case VGA_MISC_WRITE:
        {
            VgaMiscRegister = Data;

            break;
        }

        case VGA_CRTC_INDEX:
        {
            /* Set the CRTC index register */
            if (Data < VGA_CRTC_MAX_REG) VgaCrtcIndex = Data;

            break;
        }

        case VGA_CRTC_DATA:
        {
            /* Call the CRTC function */
            VgaWriteCrtc(Data);

            break;
        }

        case VGA_GC_INDEX:
        {
            /* Set the GC index register */
            if (Data < VGA_GC_MAX_REG) VgaGcIndex = Data;
            break;
        }

        case VGA_GC_DATA:
        {
            /* Call the GC function */
            VgaWriteGc(Data);

            break;
        }
    }
}

VOID VgaInitialize(HANDLE TextHandle)
{
    INT i, j;
    COORD Resolution;
    INT AddressSize;
    DWORD ScanlineSize;
    COORD Origin = { 0, 0 };
    SMALL_RECT ScreenRect;
    PCHAR_INFO CharBuffer;
    DWORD Address = 0;
    DWORD CurrentAddr;

    /* Set the global handle */
    TextConsoleBuffer = TextHandle;

    /* Clear the VGA memory */
    ZeroMemory(VgaMemory, VGA_NUM_BANKS * VGA_BANK_SIZE);

    /* Set the default video mode */
    BiosSetVideoMode(BIOS_DEFAULT_VIDEO_MODE);
    VgaUpdateMode();
    ModeChanged = FALSE;

    /* Get the data */
    Resolution = VgaGetDisplayResolution();
    CharBuffer = (PCHAR_INFO)ConsoleFramebuffer;
    AddressSize = VgaGetAddressSize();
    ScreenRect.Left = ScreenRect.Top = 0;
    ScreenRect.Right = Resolution.X;
    ScreenRect.Bottom = Resolution.Y;
    ScanlineSize = (DWORD)VgaCrtcRegisters[VGA_CRTC_OFFSET_REG] * 2;

    /* Read the data from the console into the framebuffer */
    ReadConsoleOutputA(TextConsoleBuffer,
                       CharBuffer,
                       Resolution,
                       Origin,
                       &ScreenRect);

    /* Loop through the scanlines */
    for (i = 0; i < Resolution.Y; i++)
    {
        /* Loop through the characters */
        for (j = 0; j < Resolution.X; j++)
        {
            CurrentAddr = LOWORD((Address + j) * AddressSize);

            /* Store the character in plane 0 */
            VgaMemory[CurrentAddr] = CharBuffer[i * Resolution.X + j].Char.AsciiChar;

            /* Store the attribute in plane 1 */
            VgaMemory[CurrentAddr + VGA_BANK_SIZE] = (BYTE)CharBuffer[i * Resolution.X + j].Attributes;
        }

        /* Move to the next scanline */
        Address += ScanlineSize;
    }
}

/* EOF */
