"""Flash the screen white to illuminate face for IR camera during authentication."""
import subprocess
import os
import signal

_proc = None

def start():
    """Open a fullscreen white window using Python/tkinter."""
    global _proc
    try:
        _proc = subprocess.Popen([
            "python3", "-c",
            "import tkinter as tk;"
            "r=tk.Tk();"
            "r.attributes('-fullscreen',True);"
            "r.attributes('-topmost',True);"
            "r.configure(bg='white');"
            "r.after(15000,r.destroy);"
            "r.mainloop()"
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
           env={**os.environ, 'DISPLAY': os.environ.get('DISPLAY', ':0')})
    except Exception:
        _proc = None

def stop():
    """Close the white screen."""
    global _proc
    if _proc:
        try:
            _proc.terminate()
            _proc.wait(timeout=2)
        except Exception:
            try:
                _proc.kill()
            except Exception:
                pass
        _proc = None
