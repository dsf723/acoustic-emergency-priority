"""
记录开发板串口输出（带时间戳）。用法：python serial_log.py COM10 300 [输出文件]
"""
import sys
import time

import serial


def main():
    port, secs = sys.argv[1], float(sys.argv[2])
    out = sys.argv[3] if len(sys.argv) > 3 else "serial_log.txt"
    s = serial.Serial(port, 115200, timeout=0.1)
    t0 = time.time()
    buf = b""
    with open(out, "w", encoding="utf-8") as fp:
        while time.time() - t0 < secs:
            buf += s.read(4096)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                fp.write(f"{time.time() - t0:8.2f} {line.decode(errors='replace').strip()}\n")
                fp.flush()
    s.close()


if __name__ == "__main__":
    main()
