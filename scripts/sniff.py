"""Read the serial console WITHOUT resetting the board.

`readlog.py` deliberately toggles DTR/RTS to reboot the ESP32, which is what we
want for a boot log but wrong when we need to observe a *live* device (a reset
would drop the WiFi link and any in-flight HTTP request).

Here the control lines are driven to the "run" state as soon as the port opens
and left alone, so an already-booted board keeps running and we just watch.

Usage: sniff.py [COM3] [seconds] [outfile]
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
dur = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
out = sys.argv[3] if len(sys.argv) > 3 else None

ser = serial.Serial(port, 115200, timeout=0.2, rtscts=False, dsrdtr=False)
ser.dtr = False
ser.rts = False

end = time.time() + dur
buf = b""
while time.time() < end:
    data = ser.read(4096)
    if data:
        buf += data
        sys.stdout.buffer.write(data)
        sys.stdout.flush()
ser.close()

if out:
    with open(out, "wb") as f:
        f.write(buf)
