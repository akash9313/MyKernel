import subprocess
import os
import sys
import time
import socket

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = os.path.join(BASE_DIR, 'tools', 'qemu', 'qemu-system-x86_64.exe')
OVMF = os.path.join(BASE_DIR, 'tools', 'qemu', 'share', 'edk2-x86_64-code.fd')
ESP = os.path.join(BASE_DIR, 'build', 'esp')
PPM_PATH = os.path.join(BASE_DIR, 'build', 'framebuffer.ppm')
BMP_PATH = os.path.join(BASE_DIR, 'build', 'framebuffer.bmp')

def ppm_to_bmp(ppm_file, bmp_file):
    with open(ppm_file, 'rb') as f:
        magic = f.readline().strip()
        if magic != b'P6':
            raise ValueError("Not a binary PPM (P6)")
        # Skip comments
        line = f.readline()
        while line.startswith(b'#'):
            line = f.readline()
        width, height = [int(x) for x in line.split()]
        maxval = int(f.readline().strip())
        rgb_data = f.read()

    row_stride = width * 3
    padding = (4 - (row_stride % 4)) % 4
    bmp_row_stride = row_stride + padding
    image_size = bmp_row_stride * height
    file_size = 54 + image_size

    with open(bmp_file, 'wb') as f:
        # BMP Header
        f.write(b'BM')
        f.write(file_size.to_bytes(4, 'little'))
        f.write((0).to_bytes(4, 'little'))
        f.write((54).to_bytes(4, 'little')) # Offset to pixel array

        # DIB Header (BITMAPINFOHEADER)
        f.write((40).to_bytes(4, 'little')) # Header size
        f.write(width.to_bytes(4, 'little', signed=True))
        f.write(height.to_bytes(4, 'little', signed=True)) # Bottom-to-top
        f.write((1).to_bytes(2, 'little'))  # Planes
        f.write((24).to_bytes(2, 'little')) # Bits per pixel
        f.write((0).to_bytes(4, 'little'))  # Compression (BI_RGB)
        f.write(image_size.to_bytes(4, 'little'))
        f.write((2835).to_bytes(4, 'little')) # 72 DPI
        f.write((2835).to_bytes(4, 'little'))
        f.write((0).to_bytes(4, 'little'))
        f.write((0).to_bytes(4, 'little'))

        # Pixel data (BMP is BGR, bottom-up)
        for y in range(height - 1, -1, -1):
            row_start = y * row_stride
            row_rgb = rgb_data[row_start:row_start + row_stride]
            bgr_row = bytearray()
            for x in range(0, len(row_rgb), 3):
                r = row_rgb[x]
                g = row_rgb[x+1]
                b = row_rgb[x+2]
                bgr_row.extend([b, g, r])
            bgr_row.extend(b'\x00' * padding)
            f.write(bgr_row)

    print(f"[SCREENSHOT] Successfully converted PPM to BMP: {bmp_file} ({width}x{height})")

def capture():
    # We will use QEMU TCP monitor
    port = 4445
    ppm_abs = PPM_PATH.replace('\\', '/')

    cmd = [
        QEMU,
        '-drive', f'if=pflash,format=raw,unit=0,file={OVMF},readonly=on',
        '-m', '512M',
        '-drive', f'file=fat:rw:{ESP},format=raw',
        '-serial', 'null',
        '-display', 'none',
        '-monitor', f'tcp:127.0.0.1:{port},server,nowait',
        '-no-reboot'
    ]

    print("[SCREENSHOT] Launching QEMU with TCP monitor for screendump...")
    proc = subprocess.Popen(cmd)
    time.sleep(3.5) # Wait for UEFI boot and kernel framebuffer banner

    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect(('127.0.0.1', port))
        time.sleep(0.5)
        # Read initial banner
        s.recv(1024)

        # Issue screendump command
        dump_cmd = f"screendump {ppm_abs}\n"
        print(f"[SCREENSHOT] Sending: {dump_cmd.strip()}")
        s.sendall(dump_cmd.encode('ascii'))
        time.sleep(1.0)
        s.close()
    except Exception as e:
        print(f"[WARN] Error communicating with QEMU monitor: {e}")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()

    if os.path.exists(PPM_PATH):
        print(f"[SCREENSHOT] Captured raw PPM: {PPM_PATH} ({os.path.getsize(PPM_PATH)} bytes)")
        ppm_to_bmp(PPM_PATH, BMP_PATH)
    else:
        print(f"[WARN] PPM file not created at {PPM_PATH}")

if __name__ == '__main__':
    capture()
