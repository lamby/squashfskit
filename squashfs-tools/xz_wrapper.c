/*
 * Copyright (c) 2010, 2011, 2012, 2013
 * Phillip Lougher <phillip@squashfs.org.uk>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * xz_wrapper.c
 *
 * Support for XZ (LZMA2) compression using XZ Utils liblzma
 * http://tukaani.org/xz/
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <lzma.h>

#include "squashfs_fs.h"
#include "xz_wrapper.h"
#include "compressor.h"
#include "lzma_xz_options.h"

static struct bcj bcj[] = {
	{ "x86", LZMA_FILTER_X86, 0 },
	{ "powerpc", LZMA_FILTER_POWERPC, 0 },
	{ "ia64", LZMA_FILTER_IA64, 0 },
	{ "arm", LZMA_FILTER_ARM, 0 },
	{ "armthumb", LZMA_FILTER_ARMTHUMB, 0 },
	{ "sparc", LZMA_FILTER_SPARC, 0 },
	{ NULL, LZMA_VLI_UNKNOWN, 0 }
};

static int filter_count = 1;


/*
 * This function is called by the options parsing code in mksquashfs.c
 * to parse any -X compressor option.
 *
 * Two specific options are supported:
 *	-Xbcj
 *	-Xdict-size
 *
 * This function returns:
 *	>=0 (number of additional args parsed) on success
 *	-1 if the option was unrecognised, or
 *	-2 if the option was recognised, but otherwise bad in
 *	   some way (e.g. invalid parameter)
 *
 * Note: this function sets internal compressor state, but does not
 * pass back the results of the parsing other than success/failure.
 * The xz_dump_options() function is called later to get the options in
 * a format suitable for writing to the filesystem.
 */
static int xz_options(char *argv[], int argc)
{

	if(strcmp(argv[0], "-Xbcj") == 0) {
		int i;
		char *name;
		if(argc < 2) {
			fprintf(stderr, "xz: -Xbcj missing filter\n");
			return -2;
		}

		name = argv[1];
		while(name[0] != '\0') {
			for(i = 0; bcj[i].name; i++) {
				int n = strlen(bcj[i].name);
				if((strncmp(name, bcj[i].name, n) == 0) &&
						(name[n] == '\0' ||
						 name[n] == ',')) {
					if(bcj[i].selected == 0) {
						bcj[i].selected = 1;
						filter_count++;
					}
					name += name[n] == ',' ? n + 1 : n;
					break;
				}
			}
			if(bcj[i].name == NULL) {
				fprintf(stderr, "xz: -Xbcj unrecognised "
						"filter\n");
				return -2;
			}
		}

		return 1;
	} else {
		return lzma_xz_options(argv, argc, LZMA_OPT_XZ);
	}
}


/*
 * This function is called after all options have been parsed.
 * It is used to do post-processing on the compressor options using
 * values that were not expected to be known at option parse time.
 *
 * In this case block_size may not be known until after -Xdict-size has
 * been processed (in the case where -b is specified after -Xdict-size)
 *
 * This function returns 0 on successful post processing, or
 *			-1 on error
 */
static int xz_options_post(int block_size)
{
	return lzma_xz_options_post(block_size, LZMA_OPT_XZ);
}


/*
 * This function is called by mksquashfs to dump the parsed
 * compressor options in a format suitable for writing to the
 * compressor options field in the filesystem (stored immediately
 * after the superblock).
 *
 * This function returns a pointer to the compression options structure
 * to be stored (and the size), or NULL if there are no compression
 * options
 */
static void *xz_dump_options(int block_size, int *size)
{
	int i, flags = 0;

	for(i = 0; bcj[i].name; i++)
		flags |= bcj[i].selected << i;

	return lzma_xz_dump_options(block_size, size, flags);
}


/*
 * This function is a helper specifically for the append mode of
 * mksquashfs.  Its purpose is to set the internal compressor state
 * to the stored compressor options in the passed compressor options
 * structure.
 *
 * In effect this function sets up the compressor options
 * to the same state they were when the filesystem was originally
 * generated, this is to ensure on appending, the compressor uses
 * the same compression options that were used to generate the
 * original filesystem.
 *
 * Note, even if there are no compressor options, this function is still
 * called with an empty compressor structure (size == 0), to explicitly
 * set the default options, this is to ensure any user supplied
 * -X options on the appending mksquashfs command line are over-ridden
 *
 * This function returns 0 on sucessful extraction of options, and
 *			-1 on error
 */
static int xz_extract_options(int block_size, void *buffer, int size)
{
	int ret = lzma_xz_extract_options(block_size, buffer, size, LZMA_OPT_XZ);

	if (!ret) {
		int i;
		struct lzma_xz_options *opts = lzma_xz_get_options();
		for(i = 0; bcj[i].name; i++) {
			if((opts->flags >> i) & 1) {
				bcj[i].selected = 1;
				filter_count ++;
			} else
				bcj[i].selected = 0;
		}
	}

	return ret;
}


void xz_display_options(void *buffer, int size)
{
	struct comp_opts *comp_opts = buffer;
	int dictionary_size, flags, printed;
	int i, n;

	/* check passed comp opts struct is of the correct length */
	if(size != sizeof(struct comp_opts))
		goto failed;

	SQUASHFS_INSWAP_LZMA_COMP_OPTS(comp_opts);

	dictionary_size = comp_opts->dictionary_size;
	flags = comp_opts->flags;

	/*
	 * check that the dictionary size seems correct - the dictionary
	 * size should 2^n or 2^n+2^(n+1)
	 */
	n = ffs(dictionary_size) - 1;
	if(dictionary_size != (1 << n) &&
			dictionary_size != ((1 << n) + (1 << (n + 1))))
		goto failed;

	printf("\tDictionary size %d\n", dictionary_size);

	printed = 0;
	for(i = 0; bcj[i].name; i++) {
		if((flags >> i) & 1) {
			if(printed)
				printf(", ");
			else
				printf("\tFilters selected: ");
			printf("%s", bcj[i].name);
			printed = 1;
		}
	}

	if(!printed)
		printf("\tNo filters specified\n");
	else
		printf("\n");

	return;

failed:
	fprintf(stderr, "xz: error reading stored compressor options from "
			"filesystem!\n");
}


/*
 * This function is called by mksquashfs to initialise the
 * compressor, before compress() is called.
 *
 * This function returns 0 on success, and
 *			-1 on error
 */
static int xz_init(void **strm, int block_size, int datablock)
{
	int i, j, filters = datablock ? filter_count : 1;
	struct filter *filter = malloc(filters * sizeof(struct filter));
	struct xz_stream *stream;
	struct lzma_xz_options *opts = lzma_xz_get_options();

	if(filter == NULL)
		goto failed;

	stream = *strm = malloc(sizeof(struct xz_stream));
	if(stream == NULL)
		goto failed2;

	stream->filter = filter;
	stream->filters = filters;

	memset(filter, 0, filters * sizeof(struct filter));

	stream->dictionary_size = datablock ? opts->dict_size :
							  SQUASHFS_METADATA_SIZE;

	filter[0].filter[0].id = LZMA_FILTER_LZMA2;
	filter[0].filter[0].options = &stream->opt;
	filter[0].filter[1].id = LZMA_VLI_UNKNOWN;

	for(i = 0, j = 1; datablock && bcj[i].name; i++) {
		if(bcj[i].selected) {
			filter[j].buffer = malloc(block_size);
			if(filter[j].buffer == NULL)
				goto failed3;
			filter[j].filter[0].id = bcj[i].id;
			filter[j].filter[1].id = LZMA_FILTER_LZMA2;
			filter[j].filter[1].options = &stream->opt;
			filter[j].filter[2].id = LZMA_VLI_UNKNOWN;
			j++;
		}
	}

	return 0;

failed3:
	for(i = 1; i < filters; i++)
		free(filter[i].buffer);
	free(stream);

failed2:
	free(filter);

failed:
	return -1;
}


static int xz_compress(void *strm, void *dest, void *src,  int size,
					   int block_size, int *error)
{
	int i;
	lzma_ret res = 0;
	struct xz_stream *stream = strm;
	struct filter *selected = NULL;
	struct lzma_xz_options *opts = lzma_xz_get_options();

	stream->filter[0].buffer = dest;

	for(i = 0; i < stream->filters; i++) {
		uint32_t preset = opts->preset;
		struct filter *filter = &stream->filter[i];

		if (opts->extreme)
			preset |= LZMA_PRESET_EXTREME;

		if(lzma_lzma_preset(&stream->opt, preset))
			goto failed;

		stream->opt.lc = opts->lc;
		stream->opt.lp = opts->lp;
		stream->opt.pb = opts->pb;
		if (opts->fb)
			stream->opt.nice_len = opts->fb;

		stream->opt.dict_size = stream->dictionary_size;

		filter->length = 0;
		res = lzma_stream_buffer_encode(filter->filter,
										LZMA_CHECK_CRC32, NULL, src, size, filter->buffer,
										&filter->length, block_size);

		if(res == LZMA_OK) {
			if(!selected || selected->length > filter->length)
				selected = filter;
		} else if(res != LZMA_BUF_ERROR)
			goto failed;
	}

	if(!selected)
		/*
		 * Output buffer overflow.  Return out of buffer space
		 */
		return 0;

	if(selected->buffer != dest)
		memcpy(dest, selected->buffer, selected->length);

	return (int) selected->length;

failed:
	/*
	 * All other errors return failure, with the compressor
	 * specific error code in *error
	 */
	*error = res;
	return -1;
}


static int xz_uncompress(void *dest, void *src, int size, int outsize,
						 int *error)
{
	size_t src_pos = 0;
	size_t dest_pos = 0;
	uint64_t memlimit = MEMLIMIT;

	lzma_ret res = lzma_stream_buffer_decode(&memlimit, 0, NULL,
				   src, &src_pos, size, dest, &dest_pos, outsize);

	if(res == LZMA_OK && size == (int) src_pos)
		return (int) dest_pos;
	else {
		*error = res;
		return -1;
	}
}


void xz_usage()
{
	lzma_xz_usage(LZMA_OPT_XZ);
	fprintf(stderr, "\t  -Xbcj filter1,filter2,...,filterN\n");
	fprintf(stderr, "\t\tCompress using filter1,filter2,...,filterN in");
	fprintf(stderr, " turn\n\t\t(in addition to no filter), and choose");
	fprintf(stderr, " the best compression.\n");
	fprintf(stderr, "\t\tAvailable filters: x86, arm, armthumb,");
	fprintf(stderr, " powerpc, sparc, ia64\n");
}


struct compressor xz_comp_ops = {
	.init = xz_init,
	.compress = xz_compress,
	.uncompress = xz_uncompress,
	.options = xz_options,
	.options_post = xz_options_post,
	.dump_options = xz_dump_options,
	.extract_options = xz_extract_options,
	.display_options = xz_display_options,
	.usage = xz_usage,
	.id = XZ_COMPRESSION,
	.name = "xz",
	.supported = 1
};
