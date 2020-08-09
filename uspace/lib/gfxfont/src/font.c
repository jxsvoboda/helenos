/*
 * Copyright (c) 2020 Jiri Svoboda
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** @addtogroup libgfxfont
 * @{
 */
/**
 * @file Font
 */

#include <adt/list.h>
#include <assert.h>
#include <errno.h>
#include <gfx/bitmap.h>
#include <gfx/font.h>
#include <gfx/glyph.h>
#include <mem.h>
#include <stdlib.h>
#include "../private/font.h"
#include "../private/glyph.h"

/** Initialize font metrics structure.
 *
 * Font metrics structure must always be initialized using this function
 * first.
 *
 * @param metrics Font metrics structure
 */
void gfx_font_metrics_init(gfx_font_metrics_t *metrics)
{
	memset(metrics, 0, sizeof(gfx_font_metrics_t));
}

/** Create font in graphics context.
 *
 * @param gc Graphic context
 * @param metrics Font metrics
 * @param rfont Place to store pointer to new font
 *
 * @return EOK on success, EINVAL if parameters are invald,
 *         ENOMEM if insufficient resources, EIO if graphic device connection
 *         was lost
 */
errno_t gfx_font_create(gfx_context_t *gc, gfx_font_metrics_t *metrics,
    gfx_font_t **rfont)
{
	gfx_font_t *font;
	gfx_bitmap_params_t params;
	errno_t rc;

	font = calloc(1, sizeof(gfx_font_t));
	if (font == NULL)
		return ENOMEM;

	font->gc = gc;

	rc = gfx_font_set_metrics(font, metrics);
	if (rc != EOK) {
		assert(rc == EINVAL);
		free(font);
		return rc;
	}

	/* Create font bitmap */
	gfx_bitmap_params_init(&params);
	params.rect = font->rect;

	rc = gfx_bitmap_create(font->gc, &params, NULL, &font->bitmap);
	if (rc != EOK) {
		free(font);
		return rc;
	}

	font->metrics = *metrics;
	list_initialize(&font->glyphs);
	*rfont = font;
	return EOK;
}

/** Destroy font.
 *
 * @param font Font
 */
void gfx_font_destroy(gfx_font_t *font)
{
	gfx_glyph_t *glyph;

	glyph = gfx_font_first_glyph(font);
	while (glyph != NULL) {
		gfx_glyph_destroy(glyph);
		glyph = gfx_font_first_glyph(font);
	}

	free(font);
}

/** Get font metrics.
 *
 * @param font Font
 * @param metrics Place to store metrics
 */
void gfx_font_get_metrics(gfx_font_t *font, gfx_font_metrics_t *metrics)
{
	*metrics = font->metrics;
}

/** Set font metrics.
 *
 * @param font Font
 * @param metrics Place to store metrics
 * @return EOK on success, EINVAL if supplied metrics are invalid
 */
errno_t gfx_font_set_metrics(gfx_font_t *font, gfx_font_metrics_t *metrics)
{
	font->metrics = *metrics;
	return EOK;
}

/** Get first glyph in font.
 *
 * @param font Font
 * @return First glyph or @c NULL if there are none
 */
gfx_glyph_t *gfx_font_first_glyph(gfx_font_t *font)
{
	link_t *link;

	link = list_first(&font->glyphs);
	if (link == NULL)
		return NULL;

	return list_get_instance(link, gfx_glyph_t, lglyphs);
}

/** Get next glyph in font.
 *
 * @param cur Current glyph
 * @return Next glyph in font or @c NULL if @a cur was the last one
 */
gfx_glyph_t *gfx_font_next_glyph(gfx_glyph_t *cur)
{
	link_t *link;

	link = list_next(&cur->lglyphs, &cur->font->glyphs);
	if (link == NULL)
		return NULL;

	return list_get_instance(link, gfx_glyph_t, lglyphs);
}

/** Search for glyph that should be set for the beginning of a string.
 *
 * @param font Font
 * @param str String whose beginning we would like to set
 * @param rglyph Place to store glyph that should be set
 * @param rsize Place to store number of bytes to advance in the string
 * @return EOK on success, ENOENT if no matching glyph was found
 */
int gfx_font_search_glyph(gfx_font_t *font, const char *str,
    gfx_glyph_t **rglyph, size_t *rsize)
{
	gfx_glyph_t *glyph;
	size_t msize;

	glyph = gfx_font_first_glyph(font);
	while (glyph != NULL) {
		if (gfx_glyph_matches(glyph, str, &msize)) {
			*rglyph = glyph;
			*rsize = msize;
			return EOK;
		}

		glyph = gfx_font_next_glyph(glyph);
	}

	return ENOENT;
}

/** Replace glyph graphic with empty space of specified width.
 *
 * This is used to resize a glyph in the font bitmap. This changes
 * the bitmap widht and might also make the bitmap taller.
 * Width and height of the glyph is also adjusted accordingly.
 *
 * @param font Font
 * @param glyph Glyph to replace
 * @param width Width of replacement space
 * @param height Height of replacement space
 */
errno_t gfx_font_splice_at_glyph(gfx_font_t *font, gfx_glyph_t *glyph,
    gfx_coord_t width, gfx_coord_t height)
{
	gfx_glyph_t *g;
	gfx_bitmap_t *nbitmap;
	gfx_bitmap_params_t params;
	gfx_coord_t dwidth;
	errno_t rc;

	/* Change of width of glyph */
	dwidth = width - (glyph->rect.p1.x - glyph->rect.p0.x);

	/* Create new font bitmap, wider by dwidth pixels */
	gfx_bitmap_params_init(&params);
	params.rect = font->rect;
	params.rect.p1.x += dwidth;
	if (height > params.rect.p1.y)
		params.rect.p1.y = height;

	rc = gfx_bitmap_create(font->gc, &params, NULL, &nbitmap);
	if (rc != EOK)
		goto error;

	/* Transfer glyphs before @a glyph */
	g = gfx_font_first_glyph(font);
	while (g != glyph) {
		assert(g != NULL);

		rc = gfx_glyph_transfer(g, 0, nbitmap, &params.rect);
		if (rc != EOK)
			goto error;

		g = gfx_font_next_glyph(g);
	}

	/* Skip @a glyph */
	g = gfx_font_next_glyph(g);

	/* Transfer glyphs after glyph */
	while (g != NULL) {
		rc = gfx_glyph_transfer(g, dwidth, nbitmap, &params.rect);
		if (rc != EOK)
			goto error;

		/* Update glyph coordinates */
		g->rect.p0.x += dwidth;
		g->rect.p1.x += dwidth;
		g->origin.x += dwidth;

		g = gfx_font_next_glyph(g);
	}

	/* Update glyph width and height */
	glyph->rect.p1.x = glyph->rect.p0.x + width;
	glyph->rect.p1.y = glyph->rect.p0.y + height;

	/* Update font bitmap */
	gfx_bitmap_destroy(font->bitmap);
	font->bitmap = nbitmap;
	font->rect = params.rect;

	return EOK;
error:
	if (nbitmap != NULL)
		gfx_bitmap_destroy(nbitmap);
	return rc;
}

/** @}
 */