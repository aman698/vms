"""VMS Panel Sender: pick any image on this PC and send it to the LED panel over UDP.

Run:  python vms_gui.py
"""
import socket
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, ttk

from PIL import Image, ImageDraw, ImageTk

from sender import fit_image, frame_from_image, send_frame

PREVIEW = 256        # preview boxes are PREVIEW x PREVIEW screen pixels
RESEND_MS = 1000     # keep-alive interval
FILETYPES = [("Images", "*.png *.jpg *.jpeg *.bmp *.gif *.webp"), ("All files", "*.*")]
OK, ERR = "#1a7f37", "#cf222e"


def led_preview(img):
    """Draw the panel image as round LEDs, with 4-bit colour like the real panel."""
    w, h = img.size
    cell = max(1, PREVIEW // max(w, h))
    out = Image.new("RGB", (PREVIEW, PREVIEW), (0, 0, 0))
    draw = ImageDraw.Draw(out)
    x0, y0 = (PREVIEW - w * cell) // 2, (PREVIEW - h * cell) // 2
    gap = 1 if cell >= 4 else 0
    px = img.load()
    for y in range(h):
        for x in range(w):
            r, g, b = (v & 0xF0 | v >> 4 for v in px[x, y])   # panel shows 4 bits per colour
            color = (r, g, b) if r or g or b else (24, 24, 24)  # unlit LEDs stay faintly visible
            left, top = x0 + x * cell, y0 + y * cell
            draw.ellipse((left + gap, top + gap, left + cell - 1 - gap, top + cell - 1 - gap), fill=color)
    return out


class App:
    def __init__(self, root):
        self.root = root
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.frame_id = 0
        self.frame = None    # RGB565 bytes currently on the panel, re-sent by keep-alive
        self.addr = None
        self.last_dir = str(Path.home() / "Pictures")

        root.title("VMS Panel Sender")
        root.resizable(False, False)
        main = ttk.Frame(root, padding=12)
        main.grid()

        conn = ttk.Frame(main)
        conn.grid(row=0, column=0, columnspan=2, sticky="w")
        self.ip = tk.StringVar(value="192.168.0.121")
        self.port = tk.StringVar(value="1000")
        self.width = tk.StringVar(value="32")
        self.height = tk.StringVar(value="32")
        ttk.Label(conn, text="Controller IP").pack(side="left")
        ttk.Entry(conn, textvariable=self.ip, width=15).pack(side="left", padx=(4, 12))
        ttk.Label(conn, text="Port").pack(side="left")
        ttk.Entry(conn, textvariable=self.port, width=6).pack(side="left", padx=(4, 12))
        ttk.Label(conn, text="Panel size").pack(side="left")
        ttk.Spinbox(conn, textvariable=self.width, from_=32, to=512, increment=32, width=5).pack(side="left", padx=4)
        ttk.Label(conn, text="x").pack(side="left")
        ttk.Spinbox(conn, textvariable=self.height, from_=32, to=512, increment=32, width=5).pack(side="left", padx=4)

        pick = ttk.Frame(main)
        pick.grid(row=1, column=0, columnspan=2, sticky="we", pady=10)
        self.path = tk.StringVar()
        ttk.Label(pick, text="Image").pack(side="left")
        entry = ttk.Entry(pick, textvariable=self.path, width=60)
        entry.pack(side="left", padx=4, fill="x", expand=True)
        entry.bind("<Return>", lambda e: self.load_preview())
        ttk.Button(pick, text="Browse...", command=self.browse).pack(side="left")

        blank = Image.new("RGB", (PREVIEW, PREVIEW), (32, 32, 32))
        self.orig_img = ImageTk.PhotoImage(blank)
        self.panel_img = ImageTk.PhotoImage(blank)
        ttk.Label(main, text="Original").grid(row=2, column=0)
        ttk.Label(main, text="On panel").grid(row=2, column=1)
        self.orig_view = ttk.Label(main, image=self.orig_img)
        self.orig_view.grid(row=3, column=0, padx=(0, 6))
        self.panel_view = ttk.Label(main, image=self.panel_img)
        self.panel_view.grid(row=3, column=1, padx=(6, 0))

        actions = ttk.Frame(main)
        actions.grid(row=4, column=0, columnspan=2, sticky="w", pady=(10, 4))
        ttk.Button(actions, text="Send", command=self.send).pack(side="left")
        ttk.Button(actions, text="Clear panel", command=self.clear).pack(side="left", padx=8)
        self.resend = tk.BooleanVar(value=True)
        ttk.Checkbutton(actions, text="Re-send every second", variable=self.resend).pack(side="left", padx=8)

        self.status = ttk.Label(main, text="Select an image to start")
        self.status.grid(row=5, column=0, columnspan=2, sticky="w")

        root.after(RESEND_MS, self.keep_alive)

    def set_status(self, text, color=""):
        self.status.config(text=text, foreground=color)

    def panel_size(self):
        try:
            w, h = int(self.width.get()), int(self.height.get())
            if w > 0 and h > 0:
                return w, h
        except ValueError:
            pass
        self.set_status("Panel size must be numbers like 32 x 32", ERR)
        return None

    def browse(self):
        path = filedialog.askopenfilename(title="Select image", initialdir=self.last_dir, filetypes=FILETYPES)
        if path:
            self.path.set(path)
            self.last_dir = str(Path(path).parent)
            self.load_preview()

    def selected_path(self):
        return self.path.get().strip().strip('"')   # allow paths pasted with quotes

    def load_preview(self):
        """Fit the selected image to the panel and update both previews. Returns the fitted image or None."""
        path = self.selected_path()
        if not path:
            self.set_status("Select an image first", ERR)
            return None
        size = self.panel_size()
        if size is None:
            return None
        try:
            fitted = fit_image(path, *size)
            orig = fit_image(path, PREVIEW, PREVIEW)
        except Exception as e:
            self.set_status(f"Could not open image: {e}", ERR)
            return None
        self.orig_img = ImageTk.PhotoImage(orig)
        self.panel_img = ImageTk.PhotoImage(led_preview(fitted))
        self.orig_view.config(image=self.orig_img)
        self.panel_view.config(image=self.panel_img)
        self.set_status(f"{Path(path).name} ready, press Send")
        return fitted

    def target(self):
        try:
            return self.ip.get().strip(), int(self.port.get())
        except ValueError:
            self.set_status("Port must be a number", ERR)
            return None

    def transmit(self):
        try:
            send_frame(self.sock, self.addr, self.frame_id, self.frame)
        except OSError as e:
            self.set_status(f"Send failed: {e}", ERR)
            return False
        self.frame_id = (self.frame_id + 1) & 0xFFFF
        return True

    def send(self):
        fitted = self.load_preview()
        addr = self.target()
        if fitted is None or addr is None:
            return
        self.addr, self.frame = addr, frame_from_image(fitted)
        if self.transmit():
            self.set_status(f"Sent {Path(self.selected_path()).name} to {addr[0]}:{addr[1]}", OK)

    def clear(self):
        size = self.panel_size()
        addr = self.target()
        if size is None or addr is None:
            return
        self.addr, self.frame = addr, bytes(size[0] * size[1] * 2)
        if self.transmit():
            self.set_status(f"Panel cleared ({addr[0]})", OK)

    def keep_alive(self):
        if self.resend.get() and self.frame is not None:
            self.transmit()
        self.root.after(RESEND_MS, self.keep_alive)


def main():
    try:   # sharp text on high-DPI Windows screens
        import ctypes
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        pass
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
