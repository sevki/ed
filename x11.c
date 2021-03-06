/* A GUI module for X11 */

#include <assert.h>
#include <stdio.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>
#include <X11/cursorfont.h>

#include "unicode.h"
#include "gui.h"

void die(char *);

#define FONTNAME "Monaco:pixelsize=10"

enum {
	HMargin = 16,
	VMargin = 2,
	Border = 2,
	Width = 640,
	Height = 480,
};

static Display *d;
static Visual *visual;
static Colormap cmap;
static unsigned int depth;
static int screen;
static GC gc;
static XftFont *font;
static Window win;
static Pixmap pbuf;
XftDraw *xft;
static int w, h;
static XIC xic;
static XIM xim;

static int
init()
{
	XWindowAttributes wa;
	XSetWindowAttributes swa;
	XGCValues gcv;
	Window root;
	XConfigureEvent ce;

	d = XOpenDisplay(0);
	if (!d)
		die("cannot open display");
	root = DefaultRootWindow(d);
	XGetWindowAttributes(d, root, &wa);
	visual = wa.visual;
	cmap = wa.colormap;
	screen = DefaultScreen(d);
	depth = DefaultDepth(d, screen);

	/* create the main window */
	win = XCreateSimpleWindow(d, root, 0, 0, Width, Height, 0, 0,
	                          WhitePixel(d, screen));
	swa.backing_store = WhenMapped;
	swa.bit_gravity = NorthWestGravity;
	XChangeWindowAttributes(d, win, CWBackingStore|CWBitGravity, &swa);
	XStoreName(d, win, "ED");
	XSelectInput(d, win, StructureNotifyMask|ButtonPressMask|ButtonReleaseMask|Button1MotionMask|KeyPressMask|ExposureMask|FocusChangeMask);

	/* simulate an initial resize and map the window */
	ce.type = ConfigureNotify;
	ce.width = Width;
	ce.height = Height;
	XSendEvent(d, win, False, StructureNotifyMask, (XEvent *)&ce);
	XMapWindow(d, win);

	/* input methods */
	xim = XOpenIM(d, 0, 0, 0);
	if (xim)
		xic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing|XIMStatusNothing, XNClientWindow, win, XNFocusWindow, win, NULL);

	/* allocate font */
	font = XftFontOpenName(d, screen, FONTNAME);
	if (!font)
		die("cannot open default font");

	/* initialize gc */
	gcv.foreground = WhitePixel(d, screen);
	gcv.graphics_exposures = False;
	gc = XCreateGC(d, win, GCForeground|GCGraphicsExposures, &gcv);

	/* initialize back buffer and Xft drawing context */
	pbuf = XCreatePixmap(d, win, Width, Height, depth);
	xft = XftDrawCreate(d, pbuf, visual, cmap);

	/* set the action rectangle */
	gui_x11.actionr.w = HMargin - 3;
	gui_x11.actionr.h = VMargin + font->height;

	return XConnectionNumber(d);
}

static void
fini()
{
	if (pbuf != None) {
		XftDrawDestroy(xft);
		XFreePixmap(d, pbuf);
	}
	XCloseDisplay(d);
}

static void
getfont(GFont *ret)
{
	ret->ascent = font->ascent;
	ret->descent = font->descent;
	ret->height = font->height;
}

static void
xftcolor(XftColor *xc, GColor c)
{
	xc->color.red = c.red << 8;
	xc->color.green = c.green << 8;
	xc->color.blue = c.blue << 8;
	xc->color.alpha = 65535;
	XftColorAllocValue(d, visual, cmap, &xc->color, xc);
}

static void
drawtext(GRect *clip, Rune *str, int len, int x, int y, GColor c)
{
	XftColor col;

	x += clip->x;
	y += clip->y;

	// set clip!
	xftcolor(&col, c);
	XftDrawString32(xft, &col, font, x, y, (FcChar32 *)str, len);
}

static void
drawrect(GRect *clip, int x, int y, int w, int h, GColor c)
{
	if (x + w > clip->w)
		w = clip->w - x;
	if (y + h > clip->h)
		h = clip->h - y;

	x += clip->x;
	y += clip->y;

	if (c.x) {
		XGCValues gcv;
		GC gc;

		gcv.foreground = WhitePixel(d, screen);
		gcv.function = GXxor;
		gc = XCreateGC(d, pbuf, GCFunction|GCForeground, &gcv);
		XFillRectangle(d, pbuf, gc, x, y, w, h);
		XFreeGC(d, gc);
	} else {
		XftColor col;

		xftcolor(&col, c);
		XftDrawRect(xft, &col, x, y, w, h);
	}
}

static void
drawcursor(GRect *clip, int insert, int x, int y, int w)
{
	if (insert)
		drawrect(clip, x, y, 2, font->height, GXBlack);
	else
		drawrect(clip, x, y, w, font->height, GXBlack);
}

static void
decorate(GRect *clip, int dirty, GColor c)
{
	int boxh;

	boxh = VMargin + font->height;
	drawrect(clip, HMargin-3, 0, 1, clip->h, c);
	drawrect(clip, 0, boxh, HMargin-3, 1, c);
	if (dirty)
		drawrect(clip, 2, 2, HMargin-7, boxh-4, c);
}

static void
setpointer(GPointer pt)
{
	static unsigned int map[] = {
		[GPNormal] = XC_left_ptr,
		[GPResize] = XC_fleur,
	};
	Cursor c;

	c = XCreateFontCursor(d, map[pt]);
	XDefineCursor(d, win, c);
}

static int
textwidth(Rune *str, int len)
{
	XGlyphInfo gi;

	XftTextExtents32(d, font, (FcChar32 *)str, len, &gi);
	return gi.xOff;
}

static void
sync()
{
	XCopyArea(d, pbuf, win, gc, 0, 0, w, h, 0, 0);
	XFlush(d);
}

static int
nextevent(GEvent *gev)
{
	XEvent e;

	while (XPending(d)) {

		XNextEvent(d, &e);
		if (XFilterEvent(&e, None))
			continue;
		switch (e.type) {

		case FocusIn:
			if (xic)
				XSetICFocus(xic);
			continue;

		case FocusOut:
			if (xic)
				XUnsetICFocus(xic);
			continue;

		case Expose:
			sync();
			continue;

		case ConfigureNotify:
			if (e.xconfigure.width == w)
			if (e.xconfigure.height == h)
				continue;

			w = e.xconfigure.width;
			h = e.xconfigure.height;

			pbuf = XCreatePixmap(d, win, w, h, depth);
			xft = XftDrawCreate(d, pbuf, visual, cmap);

			gev->type = GResize;
			gev->resize.width = w;
			gev->resize.height = h;
			break;

		case MotionNotify:
			gev->type = GMouseSelect;
			gev->mouse.button = GBLeft;
			gev->mouse.x = e.xmotion.x;
			gev->mouse.y = e.xmotion.y;
			break;

		case ButtonPress:
			gev->type = GMouseDown;
		if (0) {
		case ButtonRelease:
			gev->type = GMouseUp;
		}

			switch (e.xbutton.button) {
			case Button1:
				gev->mouse.button = GBLeft;
				break;
			case Button2:
				gev->mouse.button = GBMiddle;
				break;
			case Button3:
				gev->mouse.button = GBRight;
				break;
			case Button4:
				gev->mouse.button = GBWheelUp;
				break;
			case Button5:
				gev->mouse.button = GBWheelDown;
				break;
			default:
				continue;
			}

			gev->mouse.x = e.xbutton.x;
			gev->mouse.y = e.xbutton.y;
			break;

		case KeyPress:
		{
			int len;
			char buf[8];
			KeySym key;
			Status status;

			gev->type = GKey;
			if (xic)
				len = Xutf8LookupString(xic, &e.xkey, buf, 8, &key, &status);
			else
				len = XLookupString(&e.xkey, buf, 8, &key, 0);
			switch (key) {
			case XK_F1:
			case XK_F2:
			case XK_F3:
			case XK_F4:
			case XK_F5:
			case XK_F6:
			case XK_F7:
			case XK_F8:
			case XK_F9:
			case XK_F10:
			case XK_F11:
			case XK_F12:
				gev->key = GKF1 + (key - XK_F1);
				break;
			case XK_Up:
				gev->key = GKUp;
				break;
			case XK_Down:
				gev->key = GKDown;
				break;
			case XK_Left:
				gev->key = GKLeft;
				break;
			case XK_Right:
				gev->key = GKRight;
				break;
			case XK_Prior:
				gev->key = GKPageUp;
				break;
			case XK_Next:
				gev->key = GKPageDown;
				break;
			case XK_BackSpace:
				gev->key = GKBackspace;
				break;
			default:
				if (len == 0)
					continue;
				if (buf[0] == '\r')
					buf[0] = '\n';
				utf8_decode_rune(&gev->key, (unsigned char *)buf, 8);
				break;
			}
			break;
		}

		default:
			continue;
		}
		return 1;
	}
	return 0;
}

struct gui gui_x11 = {
	.init		= init,
	.fini		= fini,
	.sync		= sync,
	.decorate	= decorate,
	.drawrect	= drawrect,
	.drawcursor	= drawcursor,
	.drawtext	= drawtext,
	.getfont	= getfont,
	.nextevent	= nextevent,
	.setpointer	= setpointer,
	.textwidth	= textwidth,
	.hmargin	= HMargin,
	.vmargin	= VMargin,
	.border		= Border,
	.actionr	= {0, 0, 0, 0},
};
