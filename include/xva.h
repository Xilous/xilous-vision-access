/*
 * xva.h: the public interface of xilous-vision-access (XVA).
 *
 * XVA turns a live stream of gaze positions and switch events into standard mouse input,
 * delivered through a virtual HID mouse so that every application sees an ordinary device.
 *
 * The interface is plain C so that any language can load xva.dll: Python through ctypes,
 * C# through P/Invoke, C and C++ directly. No C++ types or exceptions cross this boundary.
 *
 * Every function is safe to call from any thread. Cursor output runs on XVA's own thread at a
 * fixed rate, so a slow or paused caller never makes the cursor stutter.
 */
#ifndef XVA_H
#define XVA_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(XVA_BUILD)
#define XVA_API __declspec(dllexport)
#else
#define XVA_API __declspec(dllimport)
#endif

typedef struct xva_ctx xva_ctx;

typedef enum xva_status {
    XVA_OK = 0,
    XVA_ERR_BAD_ARG = 1,   /* a null pointer, unknown flag, unknown button, or non-finite coordinate */
    XVA_ERR_NO_DRIVER = 2, /* no virtual HID mouse driver is installed */
    XVA_ERR_DEVICE = 3,    /* the device stopped accepting reports, for example because it was removed */
    XVA_ERR_SYSTEM = 4     /* Windows refused a thread, timer or event, or memory ran out */
} xva_status;

typedef enum xva_mouse_button {
    XVA_BUTTON_LEFT = 0,
    XVA_BUTTON_RIGHT = 1
} xva_mouse_button;

/* Run without a driver: reports are computed and then discarded. For development and tests. */
#define XVA_OPEN_DRY_RUN 0x1u

/* Opens the virtual mouse and starts the output thread. On success *out must later be passed to xva_close. */
XVA_API xva_status xva_open(xva_ctx **out, unsigned flags);

/* Stops the output thread and releases the device. Accepts null. */
XVA_API void xva_close(xva_ctx *ctx);

/*
 * Reports where the user is looking now, as fractions of the primary monitor: (0, 0) is the
 * top-left corner and (1, 1) the bottom-right. Values outside 0..1 are clamped. Send every
 * sample as it arrives; XVA keeps only the newest, so a backlog never builds up.
 */
XVA_API xva_status xva_gaze(xva_ctx *ctx, double x, double y);

/* Reports a real press (down = 1) or release (down = 0). Each call is delivered in order, immediately. */
XVA_API xva_status xva_button(xva_ctx *ctx, xva_mouse_button button, int down);

/* Scrolls by whole wheel notches: positive is up, negative is down, within -127..127. */
XVA_API xva_status xva_scroll(xva_ctx *ctx, int notches);

/* Reads the cursor position XVA last sent, in the same units as xva_gaze. For diagnostics. */
XVA_API xva_status xva_get_cursor(xva_ctx *ctx, double *x, double *y);

/* A short English description of a status code. The string is static; never free it. */
XVA_API const char *xva_status_string(xva_status status);

#ifdef __cplusplus
}
#endif

#endif /* XVA_H */
