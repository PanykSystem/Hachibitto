// =====================================================================================
// Copyright (c) 2026 Dave Bernazzani (wavemotion-dave)
//
// Copying and distribution of this emulator, its source code and associated
// readme files, with or without modification, are permitted in any medium without
// royalty provided this copyright notice is used and wavemotion-dave and
// Marat Fayzullin (fMSX core) are thanked profusely.
//
// The Hachibitto emulator is offered as-is, without any warranty. Please see readme.md
// =====================================================================================

#ifndef BOXART_PNG_H_
#define BOXART_PNG_H_

#include <nds.h>

// Load a PNG (256 pixels wide, 1..512 high) and show 192 lines of it on the top screen
// (main 2D engine, BG3, 16-bit direct color), starting at scanline yOffset (zero based).
// Returns true on success, false if the file is missing, not a PNG, not 256 wide, taller than
// 512, interlaced, or too big for the scratch buffer. If it fails before drawing begins, the
// video state is left untouched. yOffset is clamped to the valid range.
bool BoxArt_ShowPNG(const char *filename, u8 is_directory, int yOffset);

// The same thing in two steps, so you can scroll without decoding the PNG again.
// BoxArt_LoadPNG() decodes into the scratch buffer (no video changes); BoxArt_Display() draws
// 192 lines from yOffset and can be called as often as you like afterwards.
bool BoxArt_LoadPNG(const char *filename);
bool BoxArt_Display(int yOffset);

// Returns the maximum Y offset so we can smooth scroll the image up/down 
int BoxArt_MaxOffset(void);

#endif
