/*
 * Copyright (c) 2012 Petr Koupy
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

/** @addtogroup gui
 * @{
 */
/**
 * @file
 */

#include <errno.h>
#include <malloc.h>
#include <io/chargrid.h>
#include <surface.h>
#include <gfx/font-8x16.h>
#include <io/console.h>
#include <ipc/console.h>
#include <task.h>
#include <adt/list.h>
#include <adt/prodcons.h>
#include <atomic.h>
#include <stdarg.h>
#include "window.h"
#include "terminal.h"

#define NAME       "vterm"
#define NAMESPACE  "vterm"

#define LOCFS_MOUNT_POINT  "/loc"

#define APP_GETTERM  "/app/getterm"

#define TERM_CAPS \
	(CONSOLE_CAP_STYLE | CONSOLE_CAP_INDEXED | CONSOLE_CAP_RGB)

static LIST_INITIALIZE(terms);

static void getterm(const char *svc, const char *app)
{
	char term[LOC_NAME_MAXLEN];
	snprintf(term, LOC_NAME_MAXLEN, "%s/%s", LOCFS_MOUNT_POINT, svc);
	
	/* Wait for the terminal service to be ready */
	service_id_t service_id;
	int rc = loc_service_get_id(svc, &service_id, IPC_FLAG_BLOCKING);
	if (rc != EOK)
		return;
	
	task_spawnl(NULL, APP_GETTERM, APP_GETTERM, "-w", term, app, NULL);
}

static pixel_t color_table[16] = {
	[COLOR_BLACK]       = PIXEL(255, 0, 0, 0),
	[COLOR_BLUE]        = PIXEL(255, 0, 0, 240),
	[COLOR_GREEN]       = PIXEL(255, 0, 240, 0),
	[COLOR_CYAN]        = PIXEL(255, 0, 240, 240),
	[COLOR_RED]         = PIXEL(255, 240, 0, 0),
	[COLOR_MAGENTA]     = PIXEL(255, 240, 0, 240),
	[COLOR_YELLOW]      = PIXEL(255, 240, 240, 0),
	[COLOR_WHITE]       = PIXEL(255, 240, 240, 240),
	
	[COLOR_BLACK + 8]   = PIXEL(255, 0, 0, 0),
	[COLOR_BLUE + 8]    = PIXEL(255, 0, 0, 255),
	[COLOR_GREEN + 8]   = PIXEL(255, 0, 255, 0),
	[COLOR_CYAN + 8]    = PIXEL(255, 0, 255, 255),
	[COLOR_RED + 8]     = PIXEL(255, 255, 0, 0),
	[COLOR_MAGENTA + 8] = PIXEL(255, 255, 0, 255),
	[COLOR_YELLOW + 8]  = PIXEL(255, 255, 255, 0),
	[COLOR_WHITE + 8]   = PIXEL(255, 255, 255, 255),
};

static inline void attrs_rgb(char_attrs_t attrs, pixel_t *bgcolor, pixel_t *fgcolor)
{
	switch (attrs.type) {
	case CHAR_ATTR_STYLE:
		switch (attrs.val.style) {
		case STYLE_NORMAL:
			*bgcolor = color_table[COLOR_WHITE];
			*fgcolor = color_table[COLOR_BLACK];
			break;
		case STYLE_EMPHASIS:
			*bgcolor = color_table[COLOR_WHITE];
			*fgcolor = color_table[COLOR_RED];
			break;
		case STYLE_INVERTED:
			*bgcolor = color_table[COLOR_BLACK];
			*fgcolor = color_table[COLOR_WHITE];
			break;
		case STYLE_SELECTED:
			*bgcolor = color_table[COLOR_RED];
			*fgcolor = color_table[COLOR_WHITE];
			break;
		}
		break;
	case CHAR_ATTR_INDEX:
		*bgcolor = color_table[(attrs.val.index.bgcolor & 7) |
		    ((attrs.val.index.attr & CATTR_BRIGHT) ? 8 : 0)];
		*fgcolor = color_table[(attrs.val.index.fgcolor & 7) |
		    ((attrs.val.index.attr & CATTR_BRIGHT) ? 8 : 0)];
		break;
	case CHAR_ATTR_RGB:
		*bgcolor = 0xff000000 | attrs.val.rgb.bgcolor;
		*fgcolor = 0xff000000 | attrs.val.rgb.fgcolor;
		break;
	}
}

static void term_update_char(terminal_t *term, surface_t *surface,
    sysarg_t sx, sysarg_t sy, sysarg_t col, sysarg_t row)
{
	charfield_t *field =
	    chargrid_charfield_at(term->backbuf, col, row);
	
	bool inverted = chargrid_cursor_at(term->backbuf, col, row);
	
	sysarg_t bx = sx + (col * FONT_WIDTH);
	sysarg_t by = sy + (row * FONT_SCANLINES);
	
	pixel_t bgcolor = 0;
	pixel_t fgcolor = 0;
	
	if (inverted)
		attrs_rgb(field->attrs, &fgcolor, &bgcolor);
	else
		attrs_rgb(field->attrs, &bgcolor, &fgcolor);
	
	// FIXME: Glyph type should be actually uint32_t
	//        for full UTF-32 coverage.
	
	uint16_t glyph = fb_font_glyph(field->ch);
	
	// FIXME: This font drawing routine is shamelessly
	//        suboptimal. It should be optimized for
	//        aligned memory transfers, etc.
	
	for (unsigned int y = 0; y < FONT_SCANLINES; y++) {
		for (unsigned int x = 0; x < FONT_WIDTH; x++) {
			pixel_t pixel =
			    (fb_font[glyph][y] & (1 << (7 - x))) ? fgcolor : bgcolor;
			surface_put_pixel(surface, bx + x, by + y, pixel);
		}
	}
}

static bool term_update_scroll(terminal_t *term, surface_t *surface,
    sysarg_t sx, sysarg_t sy)
{
	sysarg_t top_row = chargrid_get_top_row(term->frontbuf);
	
	if (term->top_row == top_row)
		return false;
	
	term->top_row = top_row;
	
	for (sysarg_t row = 0; row < term->rows; row++) {
		for (sysarg_t col = 0; col < term->cols; col++) {
			charfield_t *front_field =
			    chargrid_charfield_at(term->frontbuf, col, row);
			charfield_t *back_field =
			    chargrid_charfield_at(term->backbuf, col, row);
			bool update = false;
			
			if (front_field->ch != back_field->ch) {
				back_field->ch = front_field->ch;
				update = true;
			}
			
			if (!attrs_same(front_field->attrs, back_field->attrs)) {
				back_field->attrs = front_field->attrs;
				update = true;
			}
			
			front_field->flags &= ~CHAR_FLAG_DIRTY;
			
			if (update)
				term_update_char(term, surface, sx, sy, col, row);
		}
	}
	
	return true;
}

static bool term_update_cursor(terminal_t *term, surface_t *surface,
    sysarg_t sx, sysarg_t sy)
{
	bool damage = false;
	
	sysarg_t front_col;
	sysarg_t front_row;
	chargrid_get_cursor(term->frontbuf, &front_col, &front_row);
	
	sysarg_t back_col;
	sysarg_t back_row;
	chargrid_get_cursor(term->backbuf, &back_col, &back_row);
	
	bool front_visibility =
	    chargrid_get_cursor_visibility(term->frontbuf);
	bool back_visibility =
	    chargrid_get_cursor_visibility(term->backbuf);
	
	if (front_visibility != back_visibility) {
		chargrid_set_cursor_visibility(term->backbuf,
		    front_visibility);
		term_update_char(term, surface, sx, sy, back_col, back_row);
		damage = true;
	}
	
	if ((front_col != back_col) || (front_row != back_row)) {
		chargrid_set_cursor(term->backbuf, front_col, front_row);
		term_update_char(term, surface, sx, sy, back_col, back_row);
		term_update_char(term, surface, sx, sy, front_col, front_row);
		damage = true;
	}
	
	return damage;
}

static void term_update(terminal_t *term)
{
	fibril_mutex_lock(&term->mtx);
	
	surface_t *surface = window_claim(term->widget.window);
	if (!surface) {
		window_yield(term->widget.window);
		fibril_mutex_unlock(&term->mtx);
		return;
	}
	
	bool damage = false;
	sysarg_t sx = term->widget.hpos;
	sysarg_t sy = term->widget.vpos;
	
	if (term_update_scroll(term, surface, sx, sy)) {
		damage = true;
	} else {
		for (sysarg_t y = 0; y < term->rows; y++) {
			for (sysarg_t x = 0; x < term->cols; x++) {
				charfield_t *front_field =
				    chargrid_charfield_at(term->frontbuf, x, y);
				charfield_t *back_field =
				    chargrid_charfield_at(term->backbuf, x, y);
				bool update = false;
				
				if ((front_field->flags & CHAR_FLAG_DIRTY) ==
				    CHAR_FLAG_DIRTY) {
					if (front_field->ch != back_field->ch) {
						back_field->ch = front_field->ch;
						update = true;
					}
					
					if (!attrs_same(front_field->attrs,
					    back_field->attrs)) {
						back_field->attrs = front_field->attrs;
						update = true;
					}
					
					front_field->flags &= ~CHAR_FLAG_DIRTY;
				}
				
				if (update) {
					term_update_char(term, surface, sx, sy, x, y);
					damage = true;
				}
			}
		}
	}
	
	if (term_update_cursor(term, surface, sx, sy))
		damage = true;
	
	window_yield(term->widget.window);
	
	if (damage)
		window_damage(term->widget.window);
	
	fibril_mutex_unlock(&term->mtx);
}

static void term_damage(terminal_t *term)
{
	fibril_mutex_lock(&term->mtx);
	
	surface_t *surface = window_claim(term->widget.window);
	if (!surface) {
		window_yield(term->widget.window);
		fibril_mutex_unlock(&term->mtx);
		return;
	}
	
	sysarg_t sx = term->widget.hpos;
	sysarg_t sy = term->widget.vpos;
	
	if (!term_update_scroll(term, surface, sx, sy)) {
		for (sysarg_t y = 0; y < term->rows; y++) {
			for (sysarg_t x = 0; x < term->cols; x++) {
				charfield_t *front_field =
				    chargrid_charfield_at(term->frontbuf, x, y);
				charfield_t *back_field =
				    chargrid_charfield_at(term->backbuf, x, y);
				
				back_field->ch = front_field->ch;
				back_field->attrs = front_field->attrs;
				front_field->flags &= ~CHAR_FLAG_DIRTY;
				
				term_update_char(term, surface, sx, sy, x, y);
			}
		}
	}
	
	term_update_cursor(term, surface, sx, sy);
	
	window_yield(term->widget.window);
	window_damage(term->widget.window);
	
	fibril_mutex_unlock(&term->mtx);
}

static void term_set_cursor(terminal_t *term, sysarg_t col, sysarg_t row)
{
	fibril_mutex_lock(&term->mtx);
	chargrid_set_cursor(term->frontbuf, col, row);
	fibril_mutex_unlock(&term->mtx);
	
	term_update(term);
}

static void term_set_cursor_visibility(terminal_t *term, bool visible)
{
	fibril_mutex_lock(&term->mtx);
	chargrid_set_cursor_visibility(term->frontbuf, visible);
	fibril_mutex_unlock(&term->mtx);
	
	term_update(term);
}

static void term_read(terminal_t *term, ipc_callid_t iid, ipc_call_t *icall)
{
	ipc_callid_t callid;
	size_t size;
	if (!async_data_read_receive(&callid, &size)) {
		async_answer_0(callid, EINVAL);
		async_answer_0(iid, EINVAL);
		return;
	}
	
	char *buf = (char *) malloc(size);
	if (buf == NULL) {
		async_answer_0(callid, ENOMEM);
		async_answer_0(iid, ENOMEM);
		return;
	}
	
	size_t pos = 0;
	
	/*
	 * Read input from keyboard and copy it to the buffer.
	 * We need to handle situation when wchar is split by 2 following
	 * reads.
	 */
	while (pos < size) {
		/* Copy to the buffer remaining characters. */
		while ((pos < size) && (term->char_remains_len > 0)) {
			buf[pos] = term->char_remains[0];
			pos++;
			
			/* Unshift the array. */
			for (size_t i = 1; i < term->char_remains_len; i++)
				term->char_remains[i - 1] = term->char_remains[i];
			
			term->char_remains_len--;
		}
		
		/* Still not enough? Then get another key from the queue. */
		if (pos < size) {
			link_t *link = prodcons_consume(&term->input_pc);
			kbd_event_t *event = list_get_instance(link, kbd_event_t, link);
			
			/* Accept key presses of printable chars only. */
			if ((event->type == KEY_PRESS) && (event->c != 0)) {
				wchar_t tmp[2] = {
					event->c,
					0
				};
				
				wstr_to_str(term->char_remains, UTF8_CHAR_BUFFER_SIZE, tmp);
				term->char_remains_len = str_size(term->char_remains);
			}
			
			free(event);
		}
	}
	
	(void) async_data_read_finalize(callid, buf, size);
	async_answer_1(iid, EOK, size);
	free(buf);
}

static void term_write_char(terminal_t *term, wchar_t ch)
{
	sysarg_t updated = 0;
	
	fibril_mutex_lock(&term->mtx);
	
	switch (ch) {
	case '\n':
		updated = chargrid_newline(term->frontbuf);
		break;
	case '\r':
		break;
	case '\t':
		updated = chargrid_tabstop(term->frontbuf, 8);
		break;
	case '\b':
		updated = chargrid_backspace(term->frontbuf);
		break;
	default:
		updated = chargrid_putchar(term->frontbuf, ch, true);
	}
	
	fibril_mutex_unlock(&term->mtx);
	
	if (updated > 1)
		term_update(term);
}

static void term_write(terminal_t *term, ipc_callid_t iid, ipc_call_t *icall)
{
	void *buf;
	size_t size;
	int rc = async_data_write_accept(&buf, false, 0, 0, 0, &size);
	
	if (rc != EOK) {
		async_answer_0(iid, rc);
		return;
	}
	
	size_t off = 0;
	while (off < size)
		term_write_char(term, str_decode(buf, &off, size));
	
	async_answer_1(iid, EOK, size);
	free(buf);
}

static void term_clear(terminal_t *term)
{
	fibril_mutex_lock(&term->mtx);
	chargrid_clear(term->frontbuf);
	fibril_mutex_unlock(&term->mtx);
	
	term_update(term);
}

static void term_get_cursor(terminal_t *term, ipc_callid_t iid, ipc_call_t *icall)
{
	sysarg_t col;
	sysarg_t row;
	
	fibril_mutex_lock(&term->mtx);
	chargrid_get_cursor(term->frontbuf, &col, &row);
	fibril_mutex_unlock(&term->mtx);
	
	async_answer_2(iid, EOK, col, row);
}

static void term_set_style(terminal_t *term, console_style_t style)
{
	fibril_mutex_lock(&term->mtx);
	chargrid_set_style(term->frontbuf, style);
	fibril_mutex_unlock(&term->mtx);
}

static void term_set_color(terminal_t *term, console_color_t bgcolor,
    console_color_t fgcolor, console_color_attr_t attr)
{
	fibril_mutex_lock(&term->mtx);
	chargrid_set_color(term->frontbuf, bgcolor, fgcolor, attr);
	fibril_mutex_unlock(&term->mtx);
}

static void term_set_rgb_color(terminal_t *term, pixel_t bgcolor,
    pixel_t fgcolor)
{
	fibril_mutex_lock(&term->mtx);
	chargrid_set_rgb_color(term->frontbuf, bgcolor, fgcolor);
	fibril_mutex_unlock(&term->mtx);
}

static void term_get_event(terminal_t *term, ipc_callid_t iid, ipc_call_t *icall)
{
	link_t *link = prodcons_consume(&term->input_pc);
	kbd_event_t *event = list_get_instance(link, kbd_event_t, link);
	
	async_answer_4(iid, EOK, event->type, event->key, event->mods, event->c);
	free(event);
}

void deinit_terminal(terminal_t *term)
{
	list_remove(&term->link);
	widget_deinit(&term->widget);
	
	if (term->frontbuf)
		chargrid_destroy(term->frontbuf);
	
	if (term->backbuf)
		chargrid_destroy(term->backbuf);
}

static void terminal_destroy(widget_t *widget)
{
	terminal_t *term = (terminal_t *) widget;
	
	deinit_terminal(term);
	free(term);
}

static void terminal_reconfigure(widget_t *widget)
{
	/* No-op */
}

static void terminal_rearrange(widget_t *widget, sysarg_t hpos, sysarg_t vpos,
    sysarg_t width, sysarg_t height)
{
	terminal_t *term = (terminal_t *) widget;
	
	widget_modify(widget, hpos, vpos, width, height);
	widget->width_ideal = width;
	widget->height_ideal = height;
	
	term_damage(term);
}

static void terminal_repaint(widget_t *widget)
{
	terminal_t *term = (terminal_t *) widget;
	
	term_damage(term);
}

static void terminal_handle_keyboard_event(widget_t *widget,
    kbd_event_t kbd_event)
{
	terminal_t *term = (terminal_t *) widget;
	
	/* Got key press/release event */
	kbd_event_t *event =
	    (kbd_event_t *) malloc(sizeof(kbd_event_t));
	if (event == NULL)
		return;
	
	link_initialize(&event->link);
	event->type = kbd_event.type;
	event->key = kbd_event.key;
	event->mods = kbd_event.mods;
	event->c = kbd_event.c;
	
	prodcons_produce(&term->input_pc, &event->link);
}

static void terminal_handle_position_event(widget_t *widget, pos_event_t event)
{
	/*
	 * Mouse events are ignored so far.
	 * There is no consumer for it.
	 */
}

static void term_connection(ipc_callid_t iid, ipc_call_t *icall, void *arg)
{
	terminal_t *term = NULL;
	
	list_foreach(terms, link) {
		terminal_t *cur = list_get_instance(link, terminal_t, link);
		
		if (cur->dsid == (service_id_t) IPC_GET_ARG1(*icall)) {
			term = cur;
			break;
		}
	}
	
	if (term == NULL) {
		async_answer_0(iid, ENOENT);
		return;
	}
	
	if (atomic_postinc(&term->refcnt) == 0)
		term_set_cursor_visibility(term, true);
	
	/* Accept the connection */
	async_answer_0(iid, EOK);
	
	while (true) {
		ipc_call_t call;
		ipc_callid_t callid = async_get_call(&call);
		
		if (!IPC_GET_IMETHOD(call))
			return;
		
		switch (IPC_GET_IMETHOD(call)) {
		case VFS_OUT_READ:
			term_read(term, callid, &call);
			break;
		case VFS_OUT_WRITE:
			term_write(term, callid, &call);
			break;
		case VFS_OUT_SYNC:
			term_update(term);
			async_answer_0(callid, EOK);
			break;
		case CONSOLE_CLEAR:
			term_clear(term);
			async_answer_0(callid, EOK);
			break;
		case CONSOLE_GOTO:
			term_set_cursor(term, IPC_GET_ARG1(call), IPC_GET_ARG2(call));
			async_answer_0(callid, EOK);
			break;
		case CONSOLE_GET_POS:
			term_get_cursor(term, callid, &call);
			break;
		case CONSOLE_GET_SIZE:
			async_answer_2(callid, EOK, term->cols, term->rows);
			break;
		case CONSOLE_GET_COLOR_CAP:
			async_answer_1(callid, EOK, TERM_CAPS);
			break;
		case CONSOLE_SET_STYLE:
			term_set_style(term, IPC_GET_ARG1(call));
			async_answer_0(callid, EOK);
			break;
		case CONSOLE_SET_COLOR:
			term_set_color(term, IPC_GET_ARG1(call), IPC_GET_ARG2(call),
			    IPC_GET_ARG3(call));
			async_answer_0(callid, EOK);
			break;
		case CONSOLE_SET_RGB_COLOR:
			term_set_rgb_color(term, IPC_GET_ARG1(call), IPC_GET_ARG2(call));
			async_answer_0(callid, EOK);
			break;
		case CONSOLE_CURSOR_VISIBILITY:
			term_set_cursor_visibility(term, IPC_GET_ARG1(call));
			async_answer_0(callid, EOK);
			break;
		case CONSOLE_GET_EVENT:
			term_get_event(term, callid, &call);
			break;
		default:
			async_answer_0(callid, EINVAL);
		}
	}
}

bool init_terminal(terminal_t *term, widget_t *parent, sysarg_t width,
    sysarg_t height)
{
	widget_init(&term->widget, parent);
	
	link_initialize(&term->link);
	fibril_mutex_initialize(&term->mtx);
	atomic_set(&term->refcnt, 0);
	
	prodcons_initialize(&term->input_pc);
	term->char_remains_len = 0;
	
	term->widget.width = width;
	term->widget.height = height;
	term->widget.width_ideal = width;
	term->widget.height_ideal = height;
	
	term->widget.destroy = terminal_destroy;
	term->widget.reconfigure = terminal_reconfigure;
	term->widget.rearrange = terminal_rearrange;
	term->widget.repaint = terminal_repaint;
	term->widget.handle_keyboard_event = terminal_handle_keyboard_event;
	term->widget.handle_position_event = terminal_handle_position_event;
	
	term->cols = width / FONT_WIDTH;
	term->rows = height / FONT_SCANLINES;
	
	term->frontbuf = NULL;
	term->backbuf = NULL;
	
	term->frontbuf = chargrid_create(term->cols, term->rows,
	    CHARGRID_FLAG_NONE);
	if (!term->frontbuf) {
		widget_deinit(&term->widget);
		return false;
	}
	
	term->backbuf = chargrid_create(term->cols, term->rows,
	    CHARGRID_FLAG_NONE);
	if (!term->backbuf) {
		widget_deinit(&term->widget);
		return false;
	}
	
	chargrid_clear(term->frontbuf);
	chargrid_clear(term->backbuf);
	term->top_row = 0;
	
	async_set_client_connection(term_connection);
	int rc = loc_server_register(NAME);
	if (rc != EOK) {
		widget_deinit(&term->widget);
		return false;
	}
	
	char vc[LOC_NAME_MAXLEN + 1];
	snprintf(vc, LOC_NAME_MAXLEN, "%s/%" PRIu64, NAMESPACE,
	    task_get_id());
	
	rc = loc_service_register(vc, &term->dsid);
	if (rc != EOK) {
		widget_deinit(&term->widget);
		return false;
	}
	
	list_append(&term->link, &terms);
	getterm(vc, "/app/bdsh");
	
	return true;
}

terminal_t *create_terminal(widget_t *parent, sysarg_t width, sysarg_t height)
{
	terminal_t *term = (terminal_t *) malloc(sizeof(terminal_t));
	if (!term)
		return NULL;
	
	bool ret = init_terminal(term, parent, width, height);
	if (!ret) {
		free(term);
		return NULL;
	}
	
	return term;
}

/** @}
 */