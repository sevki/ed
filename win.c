/*% clang -DN=3 -DWIN_TEST -Wall -g $(pkg-config --libs x11 xft) obj/{unicode,buf,edit,x11}.o % -o #
 */

#include <assert.h>
#include <string.h>

#include "unicode.h"
#include "buf.h"
#include "gui.h"
#include "win.h"

enum { RingSize = 2 }; /* bigger is (a bit) faster */
_Static_assert(RingSize >= 2, "RingSize must be at least 2");

struct lineinfo {
	int beg, len;
	unsigned sl[RingSize]; /* beginning of line offsets */
};

static void draw(W *w, GColor bg);
static void update(W *w);
static void lineinfo(W *w, unsigned off, unsigned lim, struct lineinfo *li);

static W wins[MaxWins];
static struct {
	W win;
	W *owner;
	int visible;
} tag;
static struct gui *g;
static GFont font;
static int fwidth, fheight;
static int tabw;

/* win_init - Initialize the module using [gui] as a
 * graphical backed.
 */
void
win_init(struct gui *gui)
{
	g = gui;

	g->init();
	g->getfont(&font);

	/* initialize tab width */
	tabw = TabWidth * g->textwidth((Rune[]){' '}, 1);

	/* initialize the tag */
	tag.win.eb = eb_new();

	/* the gui module does not give a way to access the screen
	 * dimension, instead, the first event generated will always
	 * be a GResize, so we can adjust these dummy values with
	 * win_resize_frame
	 */
	fwidth = fheight = 10;
}

/* win_new - Insert a new window if possible and
 * return it. In case of error (more than MaxWins
 * windows), 0 is returned.
 */
W *
win_new(EBuf *eb)
{
	W *w;

	assert(eb);

	for (w=wins;; w++) {
		if (w - wins >= MaxWins)
			return 0;
		if (!w->eb)
			break;
	}

	w->eb = eb;
	w->gr = (GRect){0, 0, fwidth, fheight};
	w->hrig = 500;

	return w;
}

/* win_delete - Delete a window created by win_new.
 */
void
win_delete(W *w)
{
	assert(w >= wins);
	assert(w < wins+MaxWins);

	memset(w, 0, sizeof(W));
	w->eb = 0;
}

/* win_move - Position and redraw a given window, if one
 * dimension is null, simply redraw the window.
 */
void
win_move(W *pw, int x, int w, int h)
{
	GColor bg;

	if (w!=0 && h!=0) {
		pw->gr = (GRect){x, 0, w, h};
		pw->nl = (h - VMargin) / font.height;
		assert(pw->nl < MaxHeight);
	}

	if (pw == &tag.win)
		bg = GPaleGreen;
	else
		bg = GPaleYellow;

	update(pw);
	draw(pw, bg);

	if (tag.visible && tag.owner == pw)
		win_move(&tag.win, 0, 0, 0);
}

/* win_resize_frame - Called when the whole frame
 * is resized.
 */
void
win_resize_frame(int w, int h)
{
	int x, ww, rig;
	W *pw;

	if (w!=0 && h!=0) {
		fwidth = w;
		fheight = h;
	}

	for (rig=0, pw=wins; pw-wins<MaxWins; pw++)
		rig += pw->hrig;

	for (x=0, pw=wins; pw-wins<MaxWins; pw++)
		if (pw->eb) {
			ww = (fwidth * pw->hrig) / rig;
			win_move(pw, x, ww, fheight);
			if (tag.visible && tag.owner == pw) {
				tag.visible = 0;
				win_tag_toggle(pw);
			}
			x += ww;
		}
}

/* win_redraw_frame - Redraw the whole frame.
 */
void
win_redraw_frame()
{
	win_resize_frame(0, 0);
}

/* win_scroll - Scroll the window by [n] lines.
 * If [n] is negative it will scroll backwards.
 */
void
win_scroll(W *w, int n)
{
	struct lineinfo li;
	unsigned start, bol;

	if (n == 0)
		return;

	start = w->l[0];

	if (n < 0) {
		do {
			int top;

			if (start == 0)
				/* already at the top */
				break;
			bol = buf_bol(&w->eb->b, start-1);

			lineinfo(w, bol, start-1, &li);
			top = li.len - 1;
			assert(top >= 0);
			for (; n<0 && top>=0; top--, n++) {
				start = li.sl[(li.beg + top) % RingSize];
				assert(start < w->l[0]);
			}
		} while (n<0);
	} else {
		do {
			int top;

			lineinfo(w, start, -1, &li);
			top = 1;
			assert(top < li.len);
			for (; n>0 && top<li.len; top++, n--) {
				start = li.sl[(li.beg + top) % RingSize];
				assert(start > w->l[0] || w->l[0] >= w->eb->b.limbo);
			}
		} while (n>0);
	}

	w->l[0] = start;
	update(w);
}

/* win_show_cursor - Find the cursor in [w] and adjust
 * the text view so that the cursor is displayed on the
 * screen. Depending on [where], the cursor will be
 * displayed at the top or at the bottom of the screen.
 */
void
win_show_cursor(W *w, enum CursorLoc where)
{
	struct lineinfo li;
	unsigned bol;

	bol = buf_bol(&w->eb->b, w->cu);
	lineinfo(w, bol, w->cu, &li);
	assert(li.len > 0);
	w->l[0] = li.sl[(li.beg + li.len-1) % RingSize];
	if (where == CBot)
		win_scroll(w, -w->nl + 1);
	else if (where == CMid)
		win_scroll(w, -w->nl / 2);
	else
		update(w);
}

W *
win_tag_win()
{
	return &tag.win;
}

W *
win_tag_owner()
{
	assert(tag.visible);
	return tag.owner;
}

void
win_tag_toggle(W *w)
{
	if (tag.visible) {
		tag.visible = 0;
		win_move(tag.owner, 0, 0, 0);
		if (w == tag.owner)
			return;
	}

	tag.visible = 1;
	tag.owner = w;
	win_move(&tag.win, w->gr.x, w->gr.w, w->gr.h/3);

	return;
}

/* static functions */

/* runewidth - returns the width of a given
 * rune, if called on '\n', it returns 0.
 */
static int
runewidth(Rune r, int x)
{
	int rw;

	if (r == '\t') {
		rw = tabw - x % tabw;
	} else if (r == '\n') {
		rw = 0;
	} else
		rw = g->textwidth(&r, 1);

	return rw;
}

struct frag {
	Rune b[MaxWidth];
	int n;
	int x, y;
};

static void
pushfrag(struct frag *f, Rune r)
{
	assert(f->n < MaxWidth);
	f->b[f->n++] = r;
}

static void
flushfrag(struct frag *f, W *w, int x, int y)
{
	g->drawtext(&w->gr, f->b, f->n, f->x, f->y, GBlack);
	f->n = 0;
	f->x = x;
	f->y = y;
}

static void
draw(W *w, GColor bg)
{
	struct frag f;
	int x, y, cx, cy, cw, rw;
	unsigned *next, c;
	Rune r;

	g->drawrect(&w->gr, 0, 0, w->gr.w, w->gr.h, bg);

	cw = 0;
	x = HMargin;
	y = VMargin + font.ascent;
	f.n = 0;
	flushfrag(&f, w, x, y);
	next = &w->l[1];

	for (c=w->l[0]; c<w->l[w->nl]; c++) {
		if (c >= *next) {
			assert(c == *next);
			x = HMargin;
			y += font.height;
			next++;
			flushfrag(&f, w, x, y);
		}

		r = buf_get(&w->eb->b, c);
		rw = runewidth(r, x-HMargin);

		if (c == w->cu) {
			cx = x;
			cy = y - font.ascent;
			cw = rw ? rw : 4;
		}

		x += rw;
		if (r == '\t')
			flushfrag(&f, w, x, y);
		else if (r != '\n')
			pushfrag(&f, r);
	}

	flushfrag(&f, w, 0, 0);
	if (cw != 0)
		g->drawrect(&w->gr, cx, cy, cw, font.height, GXBlack);
}

static void
update(W *w)
{
	struct lineinfo li;
	int l, top;

	for (l=1; l<=w->nl;) {
		lineinfo(w, w->l[l-1], -1, &li);
		top = 1;
		assert(top<li.len);
		for (; top<li.len; top++, l++)
			w->l[l] = li.sl[(li.beg + top) % RingSize];
	}
}

static int
pushoff(struct lineinfo *li, unsigned off, int overwrite)
{
	assert(li->len <= RingSize);

	if (li->len == RingSize) {
		if (!overwrite)
			return 0;
		li->sl[li->beg] = off;
		li->beg++;
		li->beg %= RingSize;
	} else {
		int n;

		n = (li->beg + li->len) % RingSize;
		li->sl[n] = off;
		li->len++;
	}
	return 1;
}

/* lineinfo
 *
 * post: if lim != -1u, then li.len>1 at exit.
 * note: lim == off is okay, only lim will be
 * in the lineinfo offset list in this case.
 */
static void
lineinfo(W *w, unsigned off, unsigned lim, struct lineinfo *li)
{
	Rune r;
	int x, rw;

	li->beg = li->len = 0;
	x = 0;

	pushoff(li, off, lim != -1u);
	while (1) {
		r = buf_get(&w->eb->b, off);
		rw = runewidth(r, x);

		if (HMargin+x+rw > w->gr.w)
		if (x != 0) { /* force progress */
			if (pushoff(li, off, lim != -1u) == 0)
				break;
			x = 0;
			continue;
		}

		/* the termination check is after the
		 * line length check to handle long
		 * broken lines, if we don't do this,
		 * line breaks are undetected in the
		 * following configuration:
		 *
		 * |xxxxxxxxx|
		 * |xxxxxx\n |
		 * |^        |
		 *   lim
		 */
		if (off >= lim)
			break;

		x += rw;
		off++;

		if (r == '\n') {
			if (pushoff(li, off, lim != -1u) == 0)
				break;
			x = 0;
		}
	}
}


#ifdef WIN_TEST
/* test */

#include <stdlib.h>

void die(char *m) { exit(1); }

int main()
{
	GEvent e;
	EBuf *eb;
	W *ws[N], *w;
	enum CursorLoc cloc;
	unsigned char s[] =
	"je suis\n"
	"x\tQcar\n"
	"tab\ttest\n"
	"une longue longue longue longue longue longue longue longue longue longue longue longue longue ligne\n"
	"un peu d'unicode: ä æ ç\n"
	"et voila!\n\n";

	eb = eb_new();
	win_init(&gui_x11);
	for (int i = 0; i < N; i++)
		ws[i] = win_new(eb);
	w = ws[0];

	for (int i=0; i<5; i++)
		eb_ins_utf8(eb, 0, s, sizeof s - 1);
	for (int i=0; i<5; i++)
		eb_ins_utf8(tag.win.eb, 0, (unsigned char *)"TAG WINDOW\n", 10);

	do {
		g->nextevent(&e);
		if (e.type == GResize)
			win_resize_frame(e.resize.width, e.resize.height);
		if (e.type == GKey) {
			int glo = 0;
			switch (e.key) {
			case 'l': ++w->cu; cloc = CBot; break;
			case 'h': if (w->cu) --w->cu; cloc = CTop; break;
			case 'e'-'a' + 1: win_scroll(w,  1); break;
			case 'y'-'a' + 1: win_scroll(w, -1); break;
			case '+': glo=1; if (w->hrig < 25000) w->hrig += 1 + w->hrig/10; break;
			case '-': glo=1; if (w->hrig > 10) w->hrig -= 1 + w->hrig/10; break;
			case 'l'-'a'+1: win_show_cursor(w, CMid); break;
			default:
				if (e.key >= '1' && e.key <= '9') {
					int n = e.key - '1';
					if (n < N)
						win_tag_toggle(&w[n]);
				}
				continue;
			}
			if (glo)
				win_redraw_frame();
			else
				win_move(w, 0, 0, 0);
			if (e.key == 'l' || e.key == 'h')
			if (w->cu < w->l[0] || w->cu >= w->l[w->nl]) {
				win_show_cursor(w, cloc);
				win_move(w, 0, 0, 0);
			}

		}
	} while (e.type != GKey || e.key != 'q');

	g->fini();
	return 0;
}

#endif
