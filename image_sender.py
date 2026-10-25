"""Send images from the images/ folder to the vms LED panel.

Put .png/.jpg/.bmp/.gif/.webp files in the images folder, run this script and type:
    send            send the newest image in the folder
    send 2          send image number 2 from 'list'
    send logo.png   send that file
    list            show the images in the folder
    clear           blank the panel
    exit            quit

The current image is re-sent every --refresh seconds, so the panel gets it back
after a lost packet or a controller reboot.

Usage:
    python image_sender.py                         # 192.168.0.121:1000, 32x32
    python image_sender.py --ip 192.168.0.122
"""
import argparse
import socket
import threading
import time
from pathlib import Path

from sender import image_to_frame, send_frame

IMAGE_EXTS = {".png", ".jpg", ".jpeg", ".bmp", ".gif", ".webp"}


class Panel:
    def __init__(self, ip, port):
        self.addr = (ip, port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.frame_id = 0
        self.current = None
        self.lock = threading.Lock()

    def show(self, data):
        with self.lock:
            self.current = data
            self._send()

    def resend(self):
        with self.lock:
            if self.current is not None:
                self._send()

    def _send(self):
        send_frame(self.sock, self.addr, self.frame_id, self.current)
        self.frame_id = (self.frame_id + 1) & 0xFFFF


def list_images(folder):
    return sorted((p for p in folder.iterdir() if p.is_file() and p.suffix.lower() in IMAGE_EXTS),
                  key=lambda p: p.name.lower())


def pick_image(folder, arg):
    images = list_images(folder)
    if not images:
        print(f"No images in {folder}")
        return None
    if not arg:
        return max(images, key=lambda p: p.stat().st_mtime)
    if arg.isdigit():
        n = int(arg)
        if 1 <= n <= len(images):
            return images[n - 1]
        print(f"No image number {n}, type 'list'")
        return None
    path = folder / arg
    if not path.is_file():
        print(f"{arg} not found in {folder}")
        return None
    return path


def keep_alive(panel, every):
    while True:
        time.sleep(every)
        panel.resend()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--ip", default="192.168.0.121")
    p.add_argument("--port", type=int, default=1000)
    p.add_argument("--width", type=int, default=32)
    p.add_argument("--height", type=int, default=32)
    p.add_argument("--folder", default=str(Path(__file__).parent / "images"))
    p.add_argument("--refresh", type=float, default=1.0, help="re-send interval in seconds, 0 = off")
    args = p.parse_args()

    folder = Path(args.folder)
    folder.mkdir(exist_ok=True)
    panel = Panel(args.ip, args.port)
    if args.refresh > 0:
        threading.Thread(target=keep_alive, args=(panel, args.refresh), daemon=True).start()

    print(f"Panel {args.ip}:{args.port} ({args.width}x{args.height}), images from {folder}")
    print("Commands: send [number|name], list, clear, exit")

    while True:
        try:
            line = input("> ").strip()
        except (EOFError, KeyboardInterrupt):
            break
        cmd, _, arg = line.partition(" ")
        cmd, arg = cmd.lower(), arg.strip()

        if cmd in ("exit", "quit"):
            break
        elif cmd in ("list", "ls"):
            images = list_images(folder)
            if not images:
                print(f"No images in {folder}")
            for i, img in enumerate(images, 1):
                print(f"  {i}. {img.name}")
        elif cmd == "clear":
            panel.show(bytes(args.width * args.height * 2))
            print("Panel cleared")
        elif cmd == "send":
            path = pick_image(folder, arg)
            if path is None:
                continue
            try:
                data = image_to_frame(path, args.width, args.height)
            except Exception as e:
                print(f"Could not read {path.name}: {e}")
                continue
            panel.show(data)
            print(f"Sent {path.name}")
        elif cmd:
            print("Commands: send [number|name], list, clear, exit")


if __name__ == "__main__":
    main()
