"""Capture ESP32 serial output to file."""
import serial, sys, time, os

PORT = 'COM9'
BAUD = 115200
DURATION = 120
OUTPUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'device.log')
# Resolve to absolute
OUTPUT = os.path.abspath(OUTPUT)

print(f"Capturing {PORT} @ {BAUD} for {DURATION}s -> {OUTPUT}")

try:
    s = serial.Serial(PORT, BAUD, timeout=0.1)
except Exception as e:
    print(f"ERROR opening {PORT}: {e}", file=sys.stderr)
    sys.exit(1)

start = time.time()
with open(OUTPUT, 'wb') as f:
    while time.time() - start < DURATION:
        data = s.read(4096)
        if data:
            f.write(data)
            f.flush()
            sys.stdout.buffer.write(data)
            sys.stdout.flush()
        else:
            time.sleep(0.05)

s.close()
print(f"\nDone. {os.path.getsize(OUTPUT)} bytes written to {OUTPUT}")
