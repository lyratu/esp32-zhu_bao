import sys, time, serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
dur = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0

ser = serial.Serial(port, 115200, timeout=1)
# toggle DTR/RTS to reset the board into normal boot
ser.dtr = False
ser.rts = True
time.sleep(0.15)
ser.rts = False
time.sleep(0.1)

end = time.time() + dur
buf = b""
while time.time() < end:
    data = ser.read(4096)
    if data:
        buf += data
        sys.stdout.buffer.write(data)
        sys.stdout.flush()
ser.close()
