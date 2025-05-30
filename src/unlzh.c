/* unlzh.c -- decompress files in SCO compress -H (LZH) format.
 * The code in this file is directly derived from the public domain 'ar002'
 * written by Haruhiko Okumura.
 */

// NOTE: for use with .a2m v12-13-14 some defines were tweaked:
// DICBIT, CBIT, PBIT, TBIT
// old values are kept in /* */ blocks

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <string.h>

#include "unlzh.h"

typedef unsigned short ush;
typedef unsigned char uch;

/* decode.c */

static unsigned	decode	(unsigned count, uch buffer[]);
static void decode_start (void);

/* huf.c */
static void huf_decode_start (void);
static unsigned decode_c		(void);
static unsigned decode_p		(void);
static void read_pt_len		(int nn, int nbit, int i_special);
static void read_c_len		(void);

/* io.c */
static void fillbuf		(int n);
static unsigned getbits	(int n);
static void init_getbits (void);

/* maketbl.c */

static void make_table (int nchar, uch bitlen[],
					   int tablebits, ush table[]);


#define DICBIT		14 /* 13 */
#define DIC_SIZE	((unsigned) 1 << DICBIT)

#ifndef CHAR_BIT
#  define CHAR_BIT 8
#endif

#ifndef UCHAR_MAX
#  define UCHAR_MAX 255
#endif

#define BITBUFSIZ (CHAR_BIT * 2 * sizeof(char))
/* Do not use CHAR_BIT * sizeof(bitbuf), does not work on machines
 * for which short is not on 16 bits (Cray).
 */

/* encode.c and decode.c */

#define MAXMATCH	256	/* formerly F (not more than UCHAR_MAX + 1) */
#define THRESHOLD	2	/* 3 */ /* choose optimal value */

/* huf.c */

#define NC			(UCHAR_MAX + MAXMATCH + 2 - THRESHOLD)
/* alphabet = {0, 1, 2, ..., NC - 1} */
#define CBIT		16 /* 9 */
#define CODE_BIT	16 /* codeword length */

#define NP			(DICBIT + 1)
#define NT			(CODE_BIT + 3)
#define PBIT		14 /* 4	*/ /* smallest integer such that (1U << PBIT) > NP */
#define TBIT		15 /* 5 */ /* smallest integer such that (1U << TBIT) > NT */
#define NPT			(1 << TBIT)

static ush left[2 * NC - 1];
static ush right[2 * NC - 1];
static uch c_len[NC];
static uch pt_len[NPT];
static unsigned blocksize;
static ush pt_table[256];
static ush c_table[4096];

/***********************************************************
		io.c -- input/output
***********************************************************/

static ush		bitbuf;
static unsigned	subbitbuf;
static int		bitcount;

uch *input_buffer, *output_buffer;
unsigned input_buffer_idx, output_buffer_idx, input_buffer_size;

static uch try_byte(void)
{
	if (input_buffer_idx < input_buffer_size)
		return input_buffer[input_buffer_idx++];
	else
		return 0;
}

void write_buf(uch *ptr, ush size)
{
	memcpy(output_buffer + output_buffer_idx, ptr, size);
	output_buffer_idx += size;
}

static void fillbuf(int n)  /* Shift bitbuf n bits left, read n bits */
{
	bitbuf <<= n;
	while (n > bitcount) {
		bitbuf |= subbitbuf << (n -= bitcount);
		subbitbuf = (unsigned)try_byte();
		if ((int)subbitbuf == EOF) subbitbuf = 0;
		bitcount = CHAR_BIT;
	}
	bitbuf |= subbitbuf >> (bitcount -= n);
}

static unsigned getbits(int n)
{
	unsigned x;

	x = bitbuf >> (BITBUFSIZ - n);	fillbuf(n);
	return x;
}

static void init_getbits(void)
{
	bitbuf = 0;	 subbitbuf = 0;	 bitcount = 0;
	fillbuf(BITBUFSIZ);
}

/***********************************************************
		maketbl.c -- make table for decoding
***********************************************************/

static void make_table(int nchar, uch bitlen[], int tablebits, ush table[])
{
	ush count[17], weight[17], start[18], *p;
	unsigned i, k, len, ch, jutbits, avail, nextcode, mask;

	for (i = 1; i <= 16; i++) count[i] = 0;
	for (i = 0; i < (unsigned)nchar; i++) count[bitlen[i]]++;

	start[1] = 0;
	for (i = 1; i <= 16; i++)
		start[i + 1] = start[i] + (count[i] << (16 - i));
	if ((start[17] & 0xffff) != 0)
		printf("Bad table\n");

	jutbits = 16 - tablebits;
	for (i = 1; i <= (unsigned)tablebits; i++) {
		start[i] >>= jutbits;
		weight[i] = (unsigned) 1 << (tablebits - i);
	}
	while (i <= 16) {
		weight[i] = (unsigned) 1 << (16 - i);
		i++;
	}

	i = start[tablebits + 1] >> jutbits;
	if (i != 0) {
		k = 1 << tablebits;
		while (i != k) table[i++] = 0;
	}

	avail = nchar;
	mask = (unsigned) 1 << (15 - tablebits);
	for (ch = 0; ch < (unsigned)nchar; ch++) {
		if ((len = bitlen[ch]) == 0) continue;
		nextcode = start[len] + weight[len];
		if (len <= (unsigned)tablebits) {
			if ((unsigned) 1 << tablebits < nextcode)
				printf("Bad table\n");
			for (i = start[len]; i < nextcode; i++) table[i] = ch;
		} else {
			k = start[len];
			p = &table[k >> jutbits];
			i = len - tablebits;
			while (i != 0) {
				if (*p == 0) {
					right[avail] = left[avail] = 0;
					*p = avail++;
				}
				if (k & mask) p = &right[*p];
				else		  p = &left[*p];
				k <<= 1;  i--;
			}
			*p = ch;
		}
		start[len] = nextcode;
	}
}

/***********************************************************
		huf.c -- static Huffman
***********************************************************/

static void read_pt_len(int nn, int nbit, int i_special)
{
	int i, c, n;
	unsigned mask;

	n = getbits(nbit);
	if (n == 0) {
		c = getbits(nbit);
		for (i = 0; i < nn; i++) pt_len[i] = 0;
		for (i = 0; i < 256; i++) pt_table[i] = c;
	} else {
		i = 0;
		while (i < n) {
			c = bitbuf >> (BITBUFSIZ - 3);
			if (c == 7) {
				mask = (unsigned) 1 << (BITBUFSIZ - 1 - 3);
				while (mask & bitbuf) {	 mask >>= 1;  c++;	}
				if (16 < c)
					printf("Bad table\n");
			}
			fillbuf((c < 7) ? 3 : c - 3);
			pt_len[i++] = c;
			if (i == i_special) {
				c = getbits(2);
				while (--c >= 0) pt_len[i++] = 0;
			}
		}
		while (i < nn) pt_len[i++] = 0;
		make_table(nn, pt_len, 8, pt_table);
	}
}

static void read_c_len(void)
{
	int i, c, n;
	unsigned mask;

	n = getbits(CBIT);
	if (n == 0) {
		c = getbits(CBIT);
		for (i = 0; i < NC; i++) c_len[i] = 0;
		for (i = 0; i < 4096; i++) c_table[i] = c;
	} else {
		i = 0;
		while (i < n) {
			c = pt_table[bitbuf >> (BITBUFSIZ - 8)];
			if (c >= NT) {
				mask = (unsigned) 1 << (BITBUFSIZ - 1 - 8);
				do {
					if (bitbuf & mask) c = right[c];
					else			   c = left [c];
					mask >>= 1;
				} while (c >= NT);
			}
			fillbuf((int) pt_len[c]);
			if (c <= 2) {
				if		(c == 0) c = 1;
				else if (c == 1) c = getbits(4) + 3;
				else			 c = getbits(CBIT) + 20;
				while (--c >= 0) c_len[i++] = 0;
			} else c_len[i++] = c - 2;
		}
		while (i < NC) c_len[i++] = 0;
		make_table(NC, c_len, 12, c_table);
	}
}

static unsigned decode_c(void)
{
	unsigned j, mask;

	if (blocksize == 0) {
		blocksize = getbits(16);
		if (blocksize == 0) {
			return NC; /* end of file */
		}
		read_pt_len(NT, TBIT, 3);
		read_c_len();
		read_pt_len(NP, PBIT, -1);
	}
	blocksize--;
	j = c_table[bitbuf >> (BITBUFSIZ - 12)];
	if (j >= NC) {
		mask = (unsigned) 1 << (BITBUFSIZ - 1 - 12);
		do {
			if (bitbuf & mask) j = right[j];
			else			   j = left [j];
			mask >>= 1;
		} while (j >= NC);
	}
	fillbuf((int) c_len[j]);
	return j;
}

static unsigned decode_p(void)
{
	unsigned j, mask;

	j = pt_table[bitbuf >> (BITBUFSIZ - 8)];
	if (j >= NP) {
		mask = (unsigned) 1 << (BITBUFSIZ - 1 - 8);
		do {
			if (bitbuf & mask) j = right[j];
			else			   j = left [j];
			mask >>= 1;
		} while (j >= NP);
	}
	fillbuf((int) pt_len[j]);
	if (j != 0) j = ((unsigned) 1 << (j - 1)) + getbits((int) (j - 1));
	return j;
}

static void huf_decode_start(void)
{
	init_getbits();	 blocksize = 0;
}

/***********************************************************
		decode.c
***********************************************************/

static int j;	/* remaining bytes to copy */
static int done; /* set at end of input */

static void decode_start()
{
	huf_decode_start();
	j = 0;
	done = 0;
}

/* Decode the input and return the number of decoded bytes put in buffer
 */
static unsigned decode(unsigned count, uch buffer[])
	/* The calling function must keep the number of
	   bytes to be processed.  This function decodes
	   either 'count' bytes or 'DIC_SIZE' bytes, whichever
	   is smaller, into the array 'buffer[]' of size
	   'DIC_SIZE' or more.
	   Call decode_start() once for each new file
	   before calling this function.
	 */
{
	static unsigned i;
	unsigned r, c;

	r = 0;
	while (--j >= 0) {
		buffer[r] = buffer[i];
		i = (i + 1) & (DIC_SIZE - 1);
		if (++r == count) return r;
	}
	for ( ; ; ) {
		c = decode_c();
		if (c == NC) {
			done = 1;
			return r;
		}
		if (c <= UCHAR_MAX) {
			buffer[r] = c;
			if (++r == count) return r;
		} else {
			j = c - (UCHAR_MAX + 1 - THRESHOLD);
			i = (r - decode_p() - 1) & (DIC_SIZE - 1);
			while (--j >= 0) {
				buffer[r] = buffer[i];
				i = (i + 1) & (DIC_SIZE - 1);
				if (++r == count) return r;
			}
		}
	}
}


/* ===========================================================================
 * Unlzh in to out. Return OK or ERROR.
 */
#if 0
int unlzh(in, out)
	int in;
	int out;
{
	unsigned n;
	ifd = in;
	ofd = out;

	decode_start();
	while (!done) {
		n = decode((unsigned) DIC_SIZE, window);
		if (!test && n > 0) {
			write_buf(out, (char*)window, n);
		}
	}
	return OK;
}
#endif

int LZH_decompress(char *source, char *dest, int source_size, int dest_size)
{
	unsigned char *ptr;
	int size_temp;
	char ultra;
	uint32_t size_unpacked = 0;
	int size;

	input_buffer = (unsigned char *)source;
	input_buffer_idx = 0;
	input_buffer_size = source_size;

	ultra = input_buffer[input_buffer_idx++] & 1;

	output_buffer = (unsigned char *)dest;
	output_buffer_idx = 0;

	//size_unpacked = *(uint32_t *)(input_buffer + input_buffer_idx);
	// Encoded data is Little-Endian
	size_unpacked = input_buffer[input_buffer_idx] |
					(input_buffer[input_buffer_idx + 1] << 8) |
					(input_buffer[input_buffer_idx + 2] << 16) |
					(input_buffer[input_buffer_idx + 3] << 24);
	input_buffer_idx += sizeof(uint32_t);

	if (ultra) {
		//WIN_SIZE = WIN_SIZE_MAX; // (1U << 15)
		//DIC_SIZE = DIC_SIZE_MAX; // (1U << 14)
    } else {
		//WIN_SIZE = WIN_SIZE_DEF; // (1U << 12)
		//DIC_SIZE = DIC_SIZE_DEF; // (1U << 13)
	}

	ptr = calloc(DIC_SIZE, 1);

	decode_start();
	size = size_unpacked;
	while ((size > 0) && dest_size) {
		if (size > (int)DIC_SIZE) {
			size_temp = DIC_SIZE;
		} else {
			size_temp = size;
		}

		decode(size_temp, ptr);
		if (dest_size >= size_temp)
		{
			write_buf(ptr, size_temp);
			dest_size -= size_temp;
		} else {
			write_buf(ptr, dest_size);
			dest_size = 0;
		}
		size -= size_temp;
    }

	free(ptr);

	return size_unpacked;
}
