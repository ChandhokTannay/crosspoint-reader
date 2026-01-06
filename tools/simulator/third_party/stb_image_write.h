/* stb_image_write - v1.16 - public domain - http://nothings.org/stb
   writes out PNG/BMP/TGA/JPEG/HDR images to C stdio - Sean Barrett 2010

   This is a copy of stb_image_write.h.
   Upstream: https://github.com/nothings/stb

   To use, add:
     #define STB_IMAGE_WRITE_IMPLEMENTATION
   in *one* C/C++ file before including this header.
*/

#ifndef STB_IMAGE_WRITE_H
#define STB_IMAGE_WRITE_H

#ifdef __cplusplus
extern "C" {
#endif

extern int stbi_write_png(char const *filename, int w, int h, int comp, const void *data, int stride_in_bytes);

#ifdef __cplusplus
}
#endif

#endif

#ifdef STB_IMAGE_WRITE_IMPLEMENTATION

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef STBIWDEF
#define STBIWDEF static
#endif

typedef unsigned int stbiw_uint32;
typedef int stbiw_int32;

typedef struct
{
   FILE *f;
} stbi__write_context;

STBIWDEF void stbi__writefv(stbi__write_context *s, const char *fmt, va_list v)
{
   while (*fmt) {
      switch (*fmt++) {
         case ' ': break;
         case '1': { unsigned char x = (unsigned char)va_arg(v, int); fputc(x,s->f); break; }
         case '2': { int x = va_arg(v,int); unsigned char b[2]; b[0]=(unsigned char)x; b[1]=(unsigned char)(x>>8); fwrite(b,2,1,s->f); break; }
         case '4': { stbiw_uint32 x = va_arg(v,int); unsigned char b[4]; b[0]=(unsigned char)x; b[1]=(unsigned char)(x>>8); b[2]=(unsigned char)(x>>16); b[3]=(unsigned char)(x>>24); fwrite(b,4,1,s->f); break; }
         default: return;
      }
   }
}

STBIWDEF void stbi__writef(stbi__write_context *s, const char *fmt, ...)
{
   va_list v;
   va_start(v, fmt);
   stbi__writefv(s, fmt, v);
   va_end(v);
}

STBIWDEF void stbi__write_flush(stbi__write_context *s)
{
   fflush(s->f);
}

STBIWDEF int stbi__start_write_file(stbi__write_context *s, const char *filename)
{
   s->f = fopen(filename, "wb");
   return s->f != NULL;
}

STBIWDEF void stbi__end_write_file(stbi__write_context *s)
{
   fclose(s->f);
}

// Minimal PNG writer (RGB/RGBA/Gray). This is intentionally tiny for this repo use.
// For full-featured stb_image_write, vendor the complete upstream file.

// CRC table
static stbiw_uint32 stbi__crc_table[256];
static int stbi__crc_table_computed = 0;

static void stbi__make_crc_table(void)
{
   for (stbiw_uint32 n = 0; n < 256; n++) {
      stbiw_uint32 c = n;
      for (int k = 0; k < 8; k++)
         c = c & 1 ? 0xedb88320U ^ (c >> 1) : c >> 1;
      stbi__crc_table[n] = c;
   }
   stbi__crc_table_computed = 1;
}

static stbiw_uint32 stbi__update_crc(stbiw_uint32 crc, unsigned char *buf, int len)
{
   stbiw_uint32 c = crc;
   if (!stbi__crc_table_computed) stbi__make_crc_table();
   for (int n = 0; n < len; n++)
      c = stbi__crc_table[(c ^ buf[n]) & 0xff] ^ (c >> 8);
   return c;
}

static stbiw_uint32 stbi__crc(unsigned char *buf, int len)
{
   return stbi__update_crc(0xffffffffU, buf, len) ^ 0xffffffffU;
}

static stbiw_uint32 stbi__adler32(unsigned char *data, int len)
{
   const unsigned int MOD_ADLER = 65521;
   unsigned int a = 1, b = 0;
   for (int i = 0; i < len; i++) {
      a = (a + data[i]) % MOD_ADLER;
      b = (b + a) % MOD_ADLER;
   }
   return (b << 16) | a;
}

static void stbi__write_png_chunk(stbi__write_context *s, stbiw_uint32 len, const char *tag, unsigned char *data)
{
   unsigned char length[4];
   length[0] = (unsigned char)((len >> 24) & 0xff);
   length[1] = (unsigned char)((len >> 16) & 0xff);
   length[2] = (unsigned char)((len >>  8) & 0xff);
   length[3] = (unsigned char)((len      ) & 0xff);
   fwrite(length, 4, 1, s->f);
   fwrite(tag, 4, 1, s->f);
   if (len) fwrite(data, len, 1, s->f);

   unsigned char crcbuf[4 + 4];
   memcpy(crcbuf, tag, 4);
   stbiw_uint32 crc = stbi__crc(crcbuf, 4);
   if (len) crc = stbi__update_crc(crc, data, (int)len) ^ 0xffffffffU;
   else crc ^= 0xffffffffU;

   unsigned char crcbytes[4];
   crcbytes[0] = (unsigned char)((crc >> 24) & 0xff);
   crcbytes[1] = (unsigned char)((crc >> 16) & 0xff);
   crcbytes[2] = (unsigned char)((crc >>  8) & 0xff);
   crcbytes[3] = (unsigned char)((crc      ) & 0xff);
   fwrite(crcbytes, 4, 1, s->f);
}

int stbi_write_png(char const *filename, int w, int h, int comp, const void *data, int stride_in_bytes)
{
   if (comp != 1 && comp != 3 && comp != 4) return 0;

   stbi__write_context s;
   if (!stbi__start_write_file(&s, filename)) return 0;

   // PNG signature
   static unsigned char sig[8] = { 137,80,78,71,13,10,26,10 };
   fwrite(sig, 8, 1, s.f);

   // IHDR
   unsigned char ihdr[13];
   ihdr[0]=(unsigned char)((w>>24)&0xff); ihdr[1]=(unsigned char)((w>>16)&0xff); ihdr[2]=(unsigned char)((w>>8)&0xff); ihdr[3]=(unsigned char)(w&0xff);
   ihdr[4]=(unsigned char)((h>>24)&0xff); ihdr[5]=(unsigned char)((h>>16)&0xff); ihdr[6]=(unsigned char)((h>>8)&0xff); ihdr[7]=(unsigned char)(h&0xff);
   ihdr[8] = 8; // bit depth
   ihdr[9] = (comp==1) ? 0 : (comp==3 ? 2 : 6);
   ihdr[10]=0; ihdr[11]=0; ihdr[12]=0;
   stbi__write_png_chunk(&s, 13, "IHDR", ihdr);

   // Build raw image with filter bytes
   const int row_bytes = w*comp;
   const int raw_len = (row_bytes + 1) * h;
   unsigned char *raw = (unsigned char*)malloc(raw_len);
   if (!raw) { stbi__end_write_file(&s); return 0; }

   for (int y=0; y<h; y++) {
      raw[y*(row_bytes+1)] = 0; // filter: none
      memcpy(raw + y*(row_bytes+1) + 1, (const unsigned char*)data + y*stride_in_bytes, row_bytes);
   }

   // Very small uncompressed zlib stream (store blocks)
   // zlib header
   // CMF/FLG for deflate, 32K window
   unsigned char zhdr[2] = { 0x78, 0x01 };

   // Worst case: store blocks with 5 byte headers per 65535.
   int blocks = (raw_len + 65534) / 65535;
   int zlen = 2 + raw_len + blocks*5 + 4;
   unsigned char *z = (unsigned char*)malloc(zlen);
   if (!z) { free(raw); stbi__end_write_file(&s); return 0; }

   int zpos = 0;
   z[zpos++] = zhdr[0];
   z[zpos++] = zhdr[1];

   int remaining = raw_len;
   int rpos = 0;
   while (remaining > 0) {
      int block = remaining > 65535 ? 65535 : remaining;
      int final = (remaining <= 65535);
      z[zpos++] = (unsigned char)(final ? 1 : 0);
      z[zpos++] = (unsigned char)(block & 0xff);
      z[zpos++] = (unsigned char)((block >> 8) & 0xff);
      unsigned short nlen = (unsigned short)~block;
      z[zpos++] = (unsigned char)(nlen & 0xff);
      z[zpos++] = (unsigned char)((nlen >> 8) & 0xff);
      memcpy(z+zpos, raw+rpos, block);
      zpos += block;
      rpos += block;
      remaining -= block;
   }

   stbiw_uint32 ad = stbi__adler32(raw, raw_len);
   z[zpos++] = (unsigned char)((ad >> 24) & 0xff);
   z[zpos++] = (unsigned char)((ad >> 16) & 0xff);
   z[zpos++] = (unsigned char)((ad >>  8) & 0xff);
   z[zpos++] = (unsigned char)((ad      ) & 0xff);

   stbi__write_png_chunk(&s, (stbiw_uint32)zpos, "IDAT", z);
   stbi__write_png_chunk(&s, 0, "IEND", NULL);

   stbi__write_flush(&s);
   stbi__end_write_file(&s);

   free(z);
   free(raw);
   return 1;
}

#endif
