import serial, time, sys
port = sys.argv[1] if len(sys.argv) > 1 else 'COM3'
ser = serial.Serial(port, 115200, timeout=0.5)
# pulse RTS -> EN to reset the chip (ESP32-C3 USB-Serial-JTAG)
ser.dtr = False
ser.rts = True
time.sleep(0.1)
ser.rts = False
t0, buf = time.time(), b''
while time.time() - t0 < 12:
    n = ser.read(4096)
    if n:
        buf += n
ser.close()
print(buf.decode('utf-8', 'replace'))
