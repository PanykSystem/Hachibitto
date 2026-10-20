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

// =====================================================================================
// Box art display helper: reads a PNG (256 wide, up to 512 high) and paints a 192-line
// window of it on the top screen, starting at any scanline desired.
//
// SELF-CONTAINED: includes its own small PNG decoder (zlib inflate + scanline unfilter),
// so there is nothing to download and nothing extra to link.
//
// Supported PNGs: 256 wide x 1..512 high, NOT interlaced, any of: palette (1/2/4/8-bit),
// grayscale (1/2/4/8/16-bit), gray+alpha, RGB and RGBA (8 or 16-bit). Alpha is ignored.
//
// RAM: the whole decoded image is kept in BOXART_BUFFER as raw scanlines, (rowbytes+1)*height
// bytes. For a 512 line image that is 131,584 bytes (8-bit palette), 393,728 (RGB) or
// 524,800 (RGBA) - save the art as palette or RGB (not RGBA) to keep this down. Because the
// image stays decoded in that buffer, changing the offset later (BoxArt_Display) costs only
// a few milliseconds and does NOT decode the PNG again. The buffer must not be reused
// between BoxArt_LoadPNG() and the last BoxArt_Display(). The compressed file is streamed,
// never loaded whole. Decode is bit-by-bit and simple: expect a fraction of a second on real
// hardware, so we call it only from the menu/loader screen.
//
// Display method: main 2D engine, BG3 as a 16-bit direct-color bitmap in VRAM bank A.
// No palette is touched, so the emulator's 256-color BG_PALETTE is left alone. This DOES
// reconfigure the main engine and overwrite VRAM bank A, so we re-run the normal emulator
// video setup afterwards.
// =====================================================================================
#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fat.h>
#include <dirent.h>
#include <unistd.h>

#include "Hachibitto.h"
#include "boxart.h"

#define BOXART_WIDTH        256
#define BOXART_HEIGHT       192             // Lines shown on the screen
#define BOXART_MAX_HEIGHT   512             // Tallest PNG accepted

// Scratch buffer that holds the decoded scanlines. It must hold (rowbytes+1)*height bytes
// (see the sizes above). SET BOXART_BUFFER_SIZE to the real size in bytes of that buffer -
// the decoder refuses any image that would not fit rather than overrun it.
extern u8 *ROM_Memory;

u8 *BOXART_BUFFER = NULL;
#define BOXART_BUFFER_SIZE  (512*1024)

#define BOXART_VRAM_BANK    VRAM_A_MAIN_BG  // 128K bank holds the 256x256x16bpp bitmap

#define PNG_IHDR  0x49484452u
#define PNG_PLTE  0x504C5445u
#define PNG_IDAT  0x49444154u
#define PNG_IEND  0x49454E44u

typedef struct
{
    FILE          *fp;
    unsigned char  buf[4096];       // Buffered file input. Also the inflate input window (see idat_refill)
    int            pos, len;
    unsigned       chunk_left;      // Bytes of the current IDAT chunk not yet handed to the bit reader
    int            in_idat;         // An IDAT chunk was started (its CRC is pending when it ends)
    int            idat_done;
    const unsigned char *ip, *iend; // Inflate input window: a run of IDAT payload bytes inside buf[]
    unsigned       bitbuf;          // Inflate bit reader (32 bits, filled a byte at a time, LSB first)
    int            bitcnt;
    int            pad;             // Zero bytes fed to the bit reader after the data ran out (truncated file)
    unsigned char *out;             // Inflate output (== the raw PNG scanlines)
    unsigned       outpos, outcap;
    int            err;
} PngState;

// ------------------------------------------------------------------------------------
// Input: buffered file bytes, plus a window onto the IDAT payload that flows across chunks
// ------------------------------------------------------------------------------------
static int raw_get(PngState *s)
{
    if (s->pos >= s->len)
    {
        s->len = (int)fread(s->buf, 1, sizeof(s->buf), s->fp);
        s->pos = 0;
        if (s->len <= 0) { s->len = 0; return -1; }
    }
    return s->buf[s->pos++];
}

static int read_u32(PngState *s, unsigned *v)
{
    unsigned r = 0;
    for (int i = 0; i < 4; i++)
    {
        int c = raw_get(s);
        if (c < 0) return -1;
        r = (r << 8) | (unsigned)c;
    }
    *v = r;
    return 0;
}

static int skip_bytes(PngState *s, unsigned n)
{
    while (n)
    {
        unsigned avail;
        if (s->pos >= s->len)
        {
            s->len = (int)fread(s->buf, 1, sizeof(s->buf), s->fp);
            s->pos = 0;
            if (s->len <= 0) { s->len = 0; return -1; }
        }
        avail = (unsigned)(s->len - s->pos);
        if (avail > n) avail = n;
        s->pos += (int)avail;
        n -= avail;
    }
    return 0;
}

// Point [ip, iend) at the next run of IDAT payload bytes (no copying - it's a window straight
// into buf[]). Called only when the previous window has been fully consumed. Returns 0 at the end
// of the compressed data.
static int idat_refill(PngState *s)
{
    for (;;)
    {
        unsigned n;
        if (s->chunk_left == 0)
        {
            unsigned len, type;
            if (s->idat_done) return 0;
            if (s->in_idat && skip_bytes(s, 4)) { s->idat_done = 1; return 0; }   // CRC of the chunk that just ended
            if (read_u32(s, &len) || read_u32(s, &type) || type != PNG_IDAT) { s->idat_done = 1; return 0; }
            s->in_idat = 1;
            s->chunk_left = len;
            continue;
        }
        if (s->pos >= s->len)
        {
            s->len = (int)fread(s->buf, 1, sizeof(s->buf), s->fp);
            s->pos = 0;
            if (s->len <= 0) { s->len = 0; s->idat_done = 1; return 0; }
        }
        n = (unsigned)(s->len - s->pos);
        if (n > s->chunk_left) n = s->chunk_left;
        s->ip   = s->buf + s->pos;
        s->iend = s->ip + n;
        s->pos += (int)n;
        s->chunk_left -= n;
        return 1;
    }
}

// ------------------------------------------------------------------------------------
// Inflate (RFC 1951). Table-driven canonical Huffman decoding with a 32-bit bit buffer:
// the common codes (<= FASTBITS long) are resolved with ONE table lookup, the rare longer
// ones fall back to a puff.c style canonical walk. The output buffer doubles as the 32K
// history window, so no separate window is needed.
// ------------------------------------------------------------------------------------
#define MAXBITS    15
#define MAXLCODES  286
#define MAXDCODES  30
#define MAXCODES   (MAXLCODES + MAXDCODES)
#define FIXLCODES  288
#define FASTBITS   9
#define FASTSIZE   (1 << FASTBITS)
#define FASTMASK   (FASTSIZE - 1)

typedef struct
{
    u16   fast[FASTSIZE];           // (symbol << 4) | codelength, indexed by the next FASTBITS input bits. 0 = not a short code
    short count[MAXBITS + 1];
    short symbol[MAXCODES];
} Huffman;

static Huffman lencode, distcode;   // static: keeps them off the (tiny) stack

static const u16 lens[29]  = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const u8  lext[29]  = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const u16 dists[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const u8  dext[30]  = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

// Ensure at least n (<= 25) valid bits are in the bit buffer. Feeds zeros after the end of the
// data and counts them in s->pad so a truncated stream can be detected afterwards.
static void need_bits(PngState *s, int n)
{
    while (s->bitcnt < n)
    {
        if (s->ip == s->iend && !idat_refill(s)) { s->pad++; s->bitcnt += 8; continue; }
        s->bitbuf |= (unsigned)(*s->ip++) << s->bitcnt;
        s->bitcnt += 8;
    }
}

static unsigned getbits(PngState *s, int n)         // n <= 16
{
    unsigned v;
    need_bits(s, n);
    v = s->bitbuf & ((1u << n) - 1);
    s->bitbuf >>= n;
    s->bitcnt -= n;
    return v;
}

// Canonical walk for codes longer than FASTBITS (needs 15 valid bits in *bbp)
static int decode_slow(unsigned *bbp, int *bcp, const Huffman *h)
{
    unsigned b = *bbp;
    int len, code = 0, first = 0, count, index = 0;
    for (len = 1; len <= MAXBITS; len++)
    {
        code |= (int)(b & 1);
        b >>= 1;
        count = h->count[len];
        if (code - count < first)
        {
            *bbp >>= len;
            *bcp -= len;
            return h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -10;
}

static int decode_sym(PngState *s, const Huffman *h)   // non-hot-path symbol decode (block headers)
{
    unsigned e;
    need_bits(s, 15);
    e = h->fast[s->bitbuf & FASTMASK];
    if (e)
    {
        int l = (int)(e & 15);
        s->bitbuf >>= l;
        s->bitcnt -= l;
        return (int)(e >> 4);
    }
    return decode_slow(&s->bitbuf, &s->bitcnt, h);
}

static int construct(Huffman *h, const short *length, int n)
{
    static short offs[MAXBITS + 1];
    int symbol, len, left;

    memset(h->fast, 0, sizeof(h->fast));
    for (len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (symbol = 0; symbol < n; symbol++) h->count[length[symbol]]++;
    if (h->count[0] == n) return 0;

    left = 1;
    for (len = 1; len <= MAXBITS; len++)
    {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }

    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (symbol = 0; symbol < n; symbol++)
        if (length[symbol] != 0) h->symbol[offs[length[symbol]]++] = (short)symbol;

    // Build the fast table. Deflate packs Huffman codes MSB first into an LSB-first bit stream,
    // so each canonical code is stored bit-reversed and replicated for every possible tail.
    {
        unsigned code = 0;
        int index = 0;
        for (len = 1; len <= FASTBITS; len++)
        {
            for (int k = 0; k < h->count[len]; k++)
            {
                unsigned c = code + (unsigned)k, r = 0, j;
                unsigned entry = ((unsigned)h->symbol[index + k] << 4) | (unsigned)len;
                for (int b = 0; b < len; b++) { r = (r << 1) | (c & 1); c >>= 1; }
                for (j = r; j < FASTSIZE; j += (1u << len)) h->fast[j] = (u16)entry;
            }
            index += h->count[len];
            code = (code + (unsigned)h->count[len]) << 1;
        }
    }
    return left;
}

// The hot loop: decode literal/length + distance symbols until end-of-block.
// The bit buffer, input pointers and output position live in locals (registers).
static int codes(PngState *s, const Huffman *lc, const Huffman *dc)
{
    unsigned bb = s->bitbuf;
    int bc = s->bitcnt;
    const unsigned char *ip = s->ip, *ie = s->iend;
    unsigned char *const out = s->out;
    unsigned op = s->outpos;
    const unsigned cap = s->outcap;
    int pad = s->pad;
    int rc = 0;

// Top the bit buffer up to at least 25 bits (never overflows a 32-bit buffer)
#define REFILL() \
    while (bc <= 24) \
    { \
        if (ip == ie) \
        { \
            s->ip = ip; \
            if (idat_refill(s)) { ip = s->ip; ie = s->iend; } \
            else { pad++; bc += 8; continue; } \
        } \
        bb |= (unsigned)(*ip++) << bc; \
        bc += 8; \
    }

    for (;;)
    {
        unsigned e;
        int sym;

        REFILL();
        if (pad && bc < pad * 8) { rc = -1; break; }       // consumed bytes that were never there: truncated

        e = lc->fast[bb & FASTMASK];
        if (e) { int l = (int)(e & 15); bb >>= l; bc -= l; sym = (int)(e >> 4); }
        else   { sym = decode_slow(&bb, &bc, lc); if (sym < 0) { rc = sym; break; } }

        if (sym < 256)                                      // literal
        {
            if (op >= cap) { rc = 1; break; }
            out[op++] = (unsigned char)sym;
            continue;
        }
        if (sym == 256) break;                              // end of block

        {
            unsigned len, dist;
            int eb;

            sym -= 257;
            if (sym >= 29) { rc = -10; break; }
            eb  = lext[sym];
            len = lens[sym] + (bb & ((1u << eb) - 1));
            bb >>= eb; bc -= eb;

            REFILL();
            e = dc->fast[bb & FASTMASK];
            if (e) { int l = (int)(e & 15); bb >>= l; bc -= l; sym = (int)(e >> 4); }
            else   { sym = decode_slow(&bb, &bc, dc); if (sym < 0) { rc = sym; break; } }
            if (sym >= 30) { rc = -10; break; }

            eb = dext[sym];
            if (bc < eb) { REFILL(); }
            dist = dists[sym] + (bb & ((1u << eb) - 1));
            bb >>= eb; bc -= eb;

            if (dist > op) { rc = -11; break; }
            if (op + len > cap) { rc = 1; break; }

            {
                unsigned char *d = out + op;
                const unsigned char *src = d - dist;
                op += len;
                if (dist == 1) { unsigned char v = *src; do { *d++ = v; } while (--len); }
                else           { do { *d++ = *src++; } while (--len); }
            }
        }
    }
#undef REFILL

    s->bitbuf = bb;
    s->bitcnt = bc;
    s->ip     = ip;
    s->iend   = ie;
    s->outpos = op;
    s->pad    = pad;
    return rc;
}

static int stored_block(PngState *s)
{
    unsigned len, nlen;
    int drop = s->bitcnt & 7;                   // Discard bits up to the next byte boundary
    s->bitbuf >>= drop;
    s->bitcnt -= drop;

    len  = getbits(s, 16);
    nlen = getbits(s, 16);
    if (len != (~nlen & 0xFFFFu)) return -2;
    if (s->outpos + len > s->outcap) return 1;

    while (len--) s->out[s->outpos++] = (unsigned char)getbits(s, 8);      // Stored blocks are rare: keep it simple
    return (s->pad && s->bitcnt < s->pad * 8) ? -1 : 0;
}

static int fixed_block(PngState *s)
{
    static short lengths[FIXLCODES];
    int symbol;

    for (symbol = 0;   symbol < 144; symbol++) lengths[symbol] = 8;
    for (; symbol < 256; symbol++) lengths[symbol] = 9;
    for (; symbol < 280; symbol++) lengths[symbol] = 7;
    for (; symbol < FIXLCODES; symbol++) lengths[symbol] = 8;
    construct(&lencode, lengths, FIXLCODES);

    for (symbol = 0; symbol < MAXDCODES; symbol++) lengths[symbol] = 5;
    construct(&distcode, lengths, MAXDCODES);

    return codes(s, &lencode, &distcode);
}

static int dynamic_block(PngState *s)
{
    static const short order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    static short lengths[MAXCODES];
    int nlen, ndist, ncode, index, err;

    nlen  = (int)getbits(s, 5) + 257;
    ndist = (int)getbits(s, 5) + 1;
    ncode = (int)getbits(s, 4) + 4;
    if (nlen > MAXLCODES || ndist > MAXDCODES) return -3;

    for (index = 0; index < ncode; index++) lengths[order[index]] = (short)getbits(s, 3);
    for (; index < 19; index++) lengths[order[index]] = 0;

    err = construct(&lencode, lengths, 19);
    if (err != 0) return -4;

    index = 0;
    while (index < nlen + ndist)
    {
        int symbol = decode_sym(s, &lencode);
        if (symbol < 0) return symbol;
        if (symbol < 16)
        {
            lengths[index++] = (short)symbol;
        }
        else
        {
            int len = 0;
            if (symbol == 16)
            {
                if (index == 0) return -5;
                len = lengths[index - 1];
                symbol = 3 + (int)getbits(s, 2);
            }
            else if (symbol == 17) symbol = 3 + (int)getbits(s, 3);
            else                   symbol = 11 + (int)getbits(s, 7);
            if (index + symbol > nlen + ndist) return -6;
            while (symbol--) lengths[index++] = (short)len;
        }
    }

    if (lengths[256] == 0) return -9;

    err = construct(&lencode, lengths, nlen);
    if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1])) return -7;

    err = construct(&distcode, lengths + nlen, ndist);
    if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1])) return -8;

    return codes(s, &lencode, &distcode);
}

static int inflate_stream(PngState *s)
{
    int last, type, err;
    do
    {
        last = (int)getbits(s, 1);
        type = (int)getbits(s, 2);
        if (s->pad && s->bitcnt < s->pad * 8) return -1;
        if      (type == 0) err = stored_block(s);
        else if (type == 1) err = fixed_block(s);
        else if (type == 2) err = dynamic_block(s);
        else                err = -1;
        if (err != 0) return err;
    } while (!last);
    return (s->pad && s->bitcnt < s->pad * 8) ? -1 : 0;     // Truncated if we used bytes that weren't in the file
}

// ------------------------------------------------------------------------------------
// PNG scanline unfiltering (in place). Each row is: 1 filter-type byte + rowbytes of data.
// ------------------------------------------------------------------------------------
static int unfilter(unsigned char *raw, int rowbytes, int height, int bpp)
{
    const unsigned char *prev = NULL;

    for (int y = 0; y < height; y++)
    {
        unsigned char *row = raw + (size_t)y * (rowbytes + 1);
        int ft = row[0];
        int i;
        row++;

        if (!prev)                                  // First row: the "row above" is all zeros
        {
            if (ft == 4) ft = 1;                    // Paeth with zeros above == Sub
            else if (ft == 2) ft = 0;               // Up with zeros above == nothing
        }

        switch (ft)
        {
            case 0:
                break;
            case 1:
                for (i = bpp; i < rowbytes; i++) row[i] = (unsigned char)(row[i] + row[i - bpp]);
                break;
            case 2:
                for (i = 0; i < rowbytes; i++) row[i] = (unsigned char)(row[i] + prev[i]);
                break;
            case 3:
                if (!prev)
                {
                    for (i = bpp; i < rowbytes; i++) row[i] = (unsigned char)(row[i] + (row[i - bpp] >> 1));
                }
                else
                {
                    for (i = 0; i < bpp; i++) row[i] = (unsigned char)(row[i] + (prev[i] >> 1));
                    for (; i < rowbytes; i++) row[i] = (unsigned char)(row[i] + ((row[i - bpp] + prev[i]) >> 1));
                }
                break;
            case 4:
                for (i = 0; i < bpp; i++) row[i] = (unsigned char)(row[i] + prev[i]);       // a = c = 0 -> predictor is b
                for (; i < rowbytes; i++)
                {
                    int a = row[i - bpp], b = prev[i], c = prev[i - bpp];
                    int pb_c = b - c, a_c = a - c;                      // pa = |b-c|, pb = |a-c|, pc = |(b-c)+(a-c)|
                    int pa = pb_c < 0 ? -pb_c : pb_c;
                    int pb = a_c  < 0 ? -a_c  : a_c;
                    int pcv = pb_c + a_c; if (pcv < 0) pcv = -pcv;
                    int pred = (pa <= pb && pa <= pcv) ? a : (pb <= pcv ? b : c);
                    row[i] = (unsigned char)(row[i] + pred);
                }
                break;
            default:
                return -1;
        }
        prev = row;
    }
    return 0;
}

#define TO_RGB15(r,g,b)  (RGB15((r) >> 3, (g) >> 3, (b) >> 3) | BIT(15))   // BIT(15) = opaque

PngState myPNG;

// What BoxArt_Display() needs to know about the image sitting in BOXART_BUFFER.
// (static, not on the stack)
static struct
{
    int  loaded;
    int  height, depth, ctype, channels, rowbytes, indexed;
    u16  pal[256];                  // RGB15 palette (also used for grayscale <= 8-bit)
    int  err;                       // Why the last BoxArt_LoadPNG() failed (see boxart.h), 0 = no error
    unsigned needed;                // Scratch buffer bytes the last image needed (valid once the header was read)
    unsigned dw, dh, ddepth, dctype, dinterlace;    // Header fields as read from the file (diagnostics)
    int      dinflate;              // Result of the inflate step (0 = good, -1 = ran out of data, others = bad stream)
    unsigned doutpos;               // Bytes the inflate step produced
    char     dbg[96];
} boxart;

// One line of diagnostics for the last BoxArt_LoadPNG(); print it on the bottom screen.
const char *BoxArt_DebugString(void)
{
    snprintf(boxart.dbg, sizeof(boxart.dbg), "err=%d %ux%u d%u c%u i%u need=%u buf=%u inf=%d out=%u",
             boxart.err, boxart.dw, boxart.dh, boxart.ddepth, boxart.dctype, boxart.dinterlace,
             boxart.needed, (unsigned)BOXART_BUFFER_SIZE, boxart.dinflate, boxart.doutpos);
    return boxart.dbg;
}

int      BoxArt_LastError(void)   { return boxart.err; }
unsigned BoxArt_BytesNeeded(void) { return boxart.needed; }

// ------------------------------------------------------------------------------------
// Step 1: decode the whole PNG into BOXART_BUFFER. Returns true on success.
// Touches no video hardware.
// ------------------------------------------------------------------------------------
bool BoxArt_LoadPNG(const char *filename)
{
    static const unsigned char png_sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    PngState      *s   = NULL;
    unsigned char *raw = NULL;
    bool           ok  = false;
    int            i;
    unsigned       width = 0, height = 0, depth = 0, ctype = 99;
    int            have_ihdr = 0, have_plte = 0;
    int            channels, rowbytes, bpp, indexed;
    
    // ------------------------------------------------------------------------------------
    // First, we need to find up to 512K of buffer space to read in the .png and keep it
    // available. On the DSi, this is a dedicated buffer... for the DS-Lite/Phat we have
    // to get more creative to find a contiguous free buffer without disturbing ROM loads.
    // ------------------------------------------------------------------------------------
    if (isDSiMode())
    {
        extern u8 *BoxArtBuffer;
        BOXART_BUFFER = BoxArtBuffer;
    }
    else
    {
        BOXART_BUFFER = NULL;

        // If the disk is not loaded, use that space...
        if (MyMedia[MEDIA_DISK].filecrc == 0)
        {
            extern u8 *DISK_Memory;
            BOXART_BUFFER = DISK_Memory;
        }
        else // Check if we have room at the back half of cart space...
        {
            extern u32 MAX_CART_SIZE_KB;
            
            if (MyMedia[MEDIA_CART2].filecrc == 0)  // Anything in Cart2?
            {
                if (MyMedia[MEDIA_CART1].filecrc == 0)  // Anything in Cart 1?
                {
                    BOXART_BUFFER = ROM_Memory + ((MAX_CART_SIZE_KB/2) * 1024);
                }
                else if (MyMedia[MEDIA_CART1].filesize <= ((MAX_CART_SIZE_KB/2) * 1024))
                {
                    BOXART_BUFFER = ROM_Memory + ((MAX_CART_SIZE_KB/2) * 1024);
                }
            }
        }
        
        if (BOXART_BUFFER == NULL) false; // Can't find the space needed... no artwork
    }    

    boxart.loaded = 0;
    boxart.err    = 1;                      // 1 = couldn't open the file (until proven otherwise)
    boxart.needed = 0;
    boxart.dw = boxart.dh = boxart.ddepth = boxart.dctype = boxart.dinterlace = 0;
    boxart.dinflate = 0;
    boxart.doutpos  = 0;

    memset(&myPNG, 0x00, sizeof(myPNG));
    s = (PngState *)&myPNG;

    s->fp = fopen(filename, "rb");
    if (!s->fp) { return false; }

    for (i = 0; i < 256; i++) boxart.pal[i] = BIT(15);     // Unused palette entries = black

    // ---- Signature ----
    boxart.err = 2;                         // 2 = not a PNG
    for (i = 0; i < 8; i++)
        if (raw_get(s) != png_sig[i]) goto done;
    boxart.err = 4;                         // 4 = unsupported or damaged header/chunks (until proven otherwise)

    // ---- Chunks up to and including the start of the first IDAT ----
    for (;;)
    {
        unsigned len, type;
        if (read_u32(s, &len) || read_u32(s, &type)) goto done;

        if (type == PNG_IHDR)
        {
            unsigned char h[13];
            if (len != 13) goto done;
            for (i = 0; i < 13; i++) { int c = raw_get(s); if (c < 0) goto done; h[i] = (unsigned char)c; }
            width  = ((unsigned)h[0] << 24) | ((unsigned)h[1] << 16) | ((unsigned)h[2] << 8) | h[3];
            height = ((unsigned)h[4] << 24) | ((unsigned)h[5] << 16) | ((unsigned)h[6] << 8) | h[7];
            depth  = h[8];
            ctype  = h[9];
            boxart.dw = width; boxart.dh = height; boxart.ddepth = depth; boxart.dctype = ctype; boxart.dinterlace = h[12];
            if (width != BOXART_WIDTH || height < 1 || height > BOXART_MAX_HEIGHT) { boxart.err = 3; goto done; }   // 3 = wrong size
            if (h[10] != 0 || h[11] != 0 || h[12] != 0) goto done;     // Compression, filter, and NO interlace
            if (ctype == 0)      { if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) goto done; }
            else if (ctype == 3) { if (depth != 1 && depth != 2 && depth != 4 && depth != 8) goto done; }
            else if (ctype == 2 || ctype == 4 || ctype == 6) { if (depth != 8 && depth != 16) goto done; }
            else goto done;
            if (skip_bytes(s, 4)) goto done;                            // CRC
            have_ihdr = 1;
        }
        else if (type == PNG_PLTE)
        {
            if (!have_ihdr || len == 0 || len > 768 || (len % 3)) goto done;
            for (i = 0; i < (int)(len / 3); i++)
            {
                int r = raw_get(s), g = raw_get(s), b = raw_get(s);
                if (r < 0 || g < 0 || b < 0) goto done;
                boxart.pal[i] = TO_RGB15(r, g, b);
            }
            have_plte = 1;
            if (skip_bytes(s, 4)) goto done;
        }
        else if (type == PNG_IDAT)
        {
            if (!have_ihdr) goto done;
            s->chunk_left = len;
            s->in_idat = 1;
            s->ip = s->iend = s->buf;                                   // Empty inflate window - idat_refill() fills it
            break;
        }
        else if (type == PNG_IEND)
        {
            goto done;
        }
        else
        {
            if (skip_bytes(s, len + 4)) goto done;                      // Ignore ancillary chunks
        }
    }

    // ---- Work out the pixel layout ----
    indexed  = (ctype == 3) || (ctype == 0 && depth <= 8);
    channels = (ctype == 2) ? 3 : (ctype == 4) ? 2 : (ctype == 6) ? 4 : 1;
    rowbytes = (int)((width * (unsigned)channels * depth + 7) / 8);
    bpp      = (int)(((unsigned)channels * depth + 7) / 8);

    if (ctype == 3 && !have_plte) goto done;
    if (ctype == 0 && depth <= 8)                                       // Grayscale -> build a gray palette
    {
        int maxv = (1 << depth) - 1;
        for (i = 0; i <= maxv; i++) { int g = (i * 255) / maxv; boxart.pal[i] = TO_RGB15(g, g, g); }
    }

    // ---- Inflate the whole image into the scanline buffer ----
    s->outcap = (unsigned)(rowbytes + 1) * height;
    boxart.needed = s->outcap;
    if (s->outcap > (unsigned)BOXART_BUFFER_SIZE) { boxart.err = 5; goto done; }   // 5 = won't fit in the buffer - refuse rather than overrun
    boxart.err = 6;                         // 6 = decompression failed / truncated data
    raw = (unsigned char *)BOXART_BUFFER;
    s->out = raw;

    {
        int cmf = (int)getbits(s, 8), flg = (int)getbits(s, 8);
        if ((cmf & 15) != 8 || (((cmf << 8) | flg) % 31) != 0 || (flg & 0x20)) goto done;
    }
    boxart.dinflate = inflate_stream(s);
    boxart.doutpos  = s->outpos;
    if (boxart.dinflate != 0 || s->err || s->outpos != s->outcap) goto done;
    boxart.err = 7;                         // 7 = damaged scanline data
    if (unfilter(raw, rowbytes, (int)height, bpp) != 0) goto done;

    boxart.height   = (int)height;
    boxart.depth    = (int)depth;
    boxart.ctype    = (int)ctype;
    boxart.channels = channels;
    boxart.rowbytes = rowbytes;
    boxart.indexed  = indexed;
    boxart.loaded   = 1;
    boxart.err      = 0;
    ok = true;
    
    swiWaitForVBlank();
    
done:
    fclose(s->fp);
    return ok;
}

int BoxArt_MaxOffset(void)
{
    if (boxart.dh <= 192) return 0;
    else return (boxart.dh-192);
}

// ------------------------------------------------------------------------------------
// Step 2: show 192 lines of the loaded image on the top screen, starting at line yOffset
// (zero based). Cheap - call it again with a new offset to scroll. An offset past the end
// is clamped so the last 192 lines are shown; an image shorter than 192 lines is padded
// with black. Returns false if no image is loaded.
// ------------------------------------------------------------------------------------
bool BoxArt_Display(int yOffset)
{
    if (!boxart.loaded) return false;
    if (BOXART_BUFFER == NULL) return false;

    int maxOffset = boxart.height - BOXART_HEIGHT;
    if (maxOffset < 0) maxOffset = 0;
    if (yOffset < 0) yOffset = 0;
    if (yOffset > maxOffset) yOffset = maxOffset;

    lcdMainOnTop();
    videoSetMode(MODE_5_2D | DISPLAY_BG3_ACTIVE);
    vramSetBankA(BOXART_VRAM_BANK);
    int bg = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    u16 *gfx = bgGetGfxPtr(bg);

    const unsigned char *raw = (const unsigned char *)BOXART_BUFFER;
    const int bps     = boxart.depth >> 3;      // Bytes per sample for the non-indexed (8/16-bit) formats
    const int stride  = boxart.channels * bps;
    const int ctype   = boxart.ctype;
    const int depth   = boxart.depth;
    int x;

    for (int y = 0; y < BOXART_HEIGHT; y++)
    {
        u16 *dst = gfx + (y * BOXART_WIDTH);
        int srcLine = yOffset + y;

        if (srcLine >= boxart.height)               // Image shorter than the screen: black below it
        {
            for (x = 0; x < BOXART_WIDTH; x++) dst[x] = BIT(15);
            continue;
        }

        const unsigned char *row = raw + (size_t)srcLine * (boxart.rowbytes + 1) + 1;

        if (boxart.indexed)
        {
            const u16 *pal = boxart.pal;
            switch (depth)
            {
                case 8: for (x = 0; x < BOXART_WIDTH; x++) dst[x] = pal[row[x]]; break;
                case 4: for (x = 0; x < BOXART_WIDTH; x++) dst[x] = pal[(row[x >> 1] >> (4 * (1 - (x & 1)))) & 0x0F]; break;
                case 2: for (x = 0; x < BOXART_WIDTH; x++) dst[x] = pal[(row[x >> 2] >> (2 * (3 - (x & 3)))) & 0x03]; break;
                default: for (x = 0; x < BOXART_WIDTH; x++) dst[x] = pal[(row[x >> 3] >> (7 - (x & 7))) & 0x01]; break;
            }
        }
        else if (ctype == 0 || ctype == 4)          // 16-bit gray, or gray+alpha: use the high byte of the gray sample
        {
            for (x = 0; x < BOXART_WIDTH; x++) { int g = row[x * stride]; dst[x] = TO_RGB15(g, g, g); }
        }
        else                                        // RGB / RGBA at 8 or 16 bits per sample (high byte of each)
        {
            for (x = 0; x < BOXART_WIDTH; x++)
            {
                const unsigned char *p = row + x * stride;
                dst[x] = TO_RGB15(p[0], p[bps], p[2 * bps]);
            }
        }
    }
    return true;
}

char box_art_filename[256];
char *box_art_base = NULL;

// -------------------------------------------------------------------------
// Store the save state file - use global config to determine where to 
// place the .png file (or, by default, it goes in the same directory 
// as the game .rom or .bin file that was loaded).
// -------------------------------------------------------------------------
void get_boxart_filename(const char *filename, u8 isDirectory)
{
    if (box_art_base == NULL) // Find the 'art' directory...
    {
        box_art_base = "";
        
        DIR* dir = opendir("/roms/art");    // See if directory exists
        if (dir)
        {
            closedir(dir);
            box_art_base = "/roms/art/";
        }
        else
        {
            DIR* dir = opendir("/roms/msx/art");    // See if directory exists
            if (dir)
            {
                closedir(dir);
                box_art_base = "/roms/msx/art/";
            }
            else
            {
                DIR* dir = opendir("/data/art");    // See if directory exists
                if (dir)
                {
                    closedir(dir);
                    box_art_base = "/data/art/";
                }
            }
        }
    }
    
    strcpy(box_art_filename, box_art_base);

    if (isDirectory)
    {
        strcat(box_art_filename, "[");
        strcat(box_art_filename, filename);
        strcat(box_art_filename, "]");
    }
    else
    {
        strcat(box_art_filename, filename);
        box_art_filename[strlen(box_art_filename)-4] = 0;
    }
    strcat(box_art_filename, ".png");
}

// ----------------------------------------------------------------------------------------------
// Convenience: load + display in one call. If directory, we expect the file to be [DirName].png
// ----------------------------------------------------------------------------------------------
bool BoxArt_ShowPNG(const char *filename, u8 is_directory, int yOffset)
{
    get_boxart_filename(filename, is_directory);
    return BoxArt_LoadPNG(box_art_filename) && BoxArt_Display(yOffset);
}

// End of file
