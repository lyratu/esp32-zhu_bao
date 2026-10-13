"""Push the PC's clock into the running monitor so we can prove/disprove that
the TLS "certificate not trusted" failure is a clock problem.

Usage:
    python setclock.py [device-ip] [http-port] [time-port]

It sleeps until just before the next whole second, sends the local epoch, and
reports the delta the device acknowledged.
"""
import socket
import sys
import time

ip = sys.argv[1] if len(sys.argv) > 1 else "192.168.5.246"
http_port = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
time_port = int(sys.argv[3]) if len(sys.argv) > 3 else 8081

now = time.time()
sleep_for = (int(now) + 1) - now          # align to the second boundary
time.sleep(sleep_for)

sent = int(time.time())
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(3)
payload = str(sent).encode()
for attempt in range(5):
    s.sendto(payload, (ip, time_port))
    try:
        data, _ = s.recvfrom(256)
        print("device ack:", data.decode(errors="replace"))
        break
    except socket.timeout:
        print(f"attempt {attempt + 1}: no ack yet")
else:
    print("no ack - is the new firmware flashed and on the LAN?")
s.close()
print(f"sent epoch {sent} ({time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(sent))})")
