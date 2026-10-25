"""Send frames to the vms LED panel over UDP.

Packet format (must match vms.ino):
    [frame id: u16 LE][byte offset in frame: u16 LE][RGB565 pixel data, low byte first]

Usage:
    python sender.py 192.168.0.121                    # moving test pattern
    python sender.py 192.168.0.121 --image logo.png   # static image (needs Pillow)
"""
import argparse
import socket
import struct
import time

CHUNK = 1024         # pixel bytes per packet, stays under one Ethernet frame
PACKET_GAP = 0.001   # pause between packets so the W5500 keeps up


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def pack_frame(pixels):
    """pixels: list of (r, g, b) in row-major order -> RGB565 bytes, low byte first."""
    return b"".join(struct.pack("<H", rgb565(r, g, b)) for r, g, b in pixels)


def test_pattern(w, h, t):
    return [((x * 255 // (w - 1) + t * 4) & 0xFF,
             y * 255 // (h - 1),
             (t * 2) & 0xFF)
            for y in range(h) for x in range(w)]


def fit_image(path, w, h):
    """Load an image and fit it into w x h on black (keeps aspect ratio). Returns an RGB image."""
    from PIL import Image, ImageOps
    with Image.open(path) as src:
        img = ImageOps.exif_transpose(src).convert("RGBA")
    img = ImageOps.pad(img, (w, h), method=Image.Resampling.LANCZOS, color=(0, 0, 0, 255))
    rgb = Image.new("RGB", (w, h))
    rgb.paste(img, mask=img.getchannel("A"))   # transparent areas become black
    return rgb


def frame_from_image(img):
    """RGB image -> RGB565 bytes, low byte first."""
    raw = img.tobytes()
    return pack_frame(zip(raw[0::3], raw[1::3], raw[2::3]))


def image_to_frame(path, w, h):
    return frame_from_image(fit_image(path, w, h))


def wait(seconds):
    # time.sleep() can oversleep by several ms on Windows; spin for short gaps instead.
    end = time.perf_counter() + seconds
    while time.perf_counter() < end:
        pass


def send_frame(sock, addr, frame_id, data):
    for off in range(0, len(data), CHUNK):
        sock.sendto(struct.pack("<HH", frame_id, off) + data[off:off + CHUNK], addr)
        wait(PACKET_GAP)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("ip")
    p.add_argument("--port", type=int, default=1000)
    p.add_argument("--width", type=int, default=32)
    p.add_argument("--height", type=int, default=32)
    p.add_argument("--fps", type=float, default=30)
    p.add_argument("--image", help="send this image instead of the test pattern")
    args = p.parse_args()

    addr = (args.ip, args.port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    image = image_to_frame(args.image, args.width, args.height) if args.image else None

    frame_id = 0
    period = 1.0 / args.fps
    print(f"Sending {args.width}x{args.height} to {args.ip}:{args.port} at {args.fps} fps, Ctrl+C to stop")
    try:
        while True:
            start = time.perf_counter()
            data = image or pack_frame(test_pattern(args.width, args.height, frame_id))
            send_frame(sock, addr, frame_id, data)
            frame_id = (frame_id + 1) & 0xFFFF
            time.sleep(max(0.0, period - (time.perf_counter() - start)))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
