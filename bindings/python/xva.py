"""Python binding for xva.dll, loaded through ctypes.

Typical use from the main application:

    with Xva("path/to/xva.dll") as xva:
        xva.gaze(x, y)                 # every gaze sample, as fractions of the primary monitor
        xva.button(Xva.LEFT, True)     # the switch was pressed
        xva.button(Xva.LEFT, False)    # the switch was released
        xva.scroll(-1)                 # one notch down

Running this file directly opens XVA in dry-run mode, which needs no driver, and prints how the
cursor follows a gaze that rests and then jumps.
"""

import ctypes
import sys
import time
from pathlib import Path


class XvaError(RuntimeError):
    pass


class Xva:
    LEFT = 0
    RIGHT = 1
    DRY_RUN = 0x1

    def __init__(self, dll_path, dry_run=False):
        lib = ctypes.CDLL(str(dll_path))
        handle = ctypes.c_void_p

        lib.xva_open.argtypes = [ctypes.POINTER(handle), ctypes.c_uint]
        lib.xva_close.argtypes = [handle]
        lib.xva_close.restype = None
        lib.xva_gaze.argtypes = [handle, ctypes.c_double, ctypes.c_double]
        lib.xva_button.argtypes = [handle, ctypes.c_int, ctypes.c_int]
        lib.xva_scroll.argtypes = [handle, ctypes.c_int]
        lib.xva_get_cursor.argtypes = [handle, ctypes.POINTER(ctypes.c_double), ctypes.POINTER(ctypes.c_double)]
        lib.xva_status_string.argtypes = [ctypes.c_int]
        lib.xva_status_string.restype = ctypes.c_char_p
        self._lib = lib

        self._ctx = handle()
        self._check(lib.xva_open(ctypes.byref(self._ctx), self.DRY_RUN if dry_run else 0))

    def _check(self, status):
        if status != 0:
            raise XvaError(self._lib.xva_status_string(status).decode())

    def gaze(self, x, y):
        self._check(self._lib.xva_gaze(self._ctx, x, y))

    def button(self, button, down):
        self._check(self._lib.xva_button(self._ctx, button, 1 if down else 0))

    def scroll(self, notches):
        self._check(self._lib.xva_scroll(self._ctx, notches))

    def cursor(self):
        x, y = ctypes.c_double(), ctypes.c_double()
        self._check(self._lib.xva_get_cursor(self._ctx, ctypes.byref(x), ctypes.byref(y)))
        return x.value, y.value

    def close(self):
        if self._ctx:
            self._lib.xva_close(self._ctx)
            self._ctx = ctypes.c_void_p()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def _dry_run(dll_path):
    with Xva(dll_path, dry_run=True) as xva:
        for target in [(0.3, 0.3), (0.8, 0.6)]:
            started = time.perf_counter()
            while time.perf_counter() - started < 0.3:
                xva.gaze(*target)
                time.sleep(1 / 60)  # a 60 Hz eye tracker
                x, y = xva.cursor()
                print(f"gaze {target}  cursor ({x:.3f}, {y:.3f})")
            if abs(x - target[0]) > 0.02 or abs(y - target[1]) > 0.02:
                sys.exit(f"cursor ({x:.3f}, {y:.3f}) did not settle on gaze {target}")
        xva.button(Xva.LEFT, True)
        xva.button(Xva.LEFT, False)
        print("clicked")


if __name__ == "__main__":
    default = Path(__file__).resolve().parents[2] / "build" / "Release" / "xva.dll"
    _dry_run(sys.argv[1] if len(sys.argv) > 1 else default)
