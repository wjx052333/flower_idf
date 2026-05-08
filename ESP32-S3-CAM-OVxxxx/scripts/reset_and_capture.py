"""Hard-reset ESP32 via RTS and capture boot + test logs."""
import serial, sys, time, os

PORT = 'COM9'
BAUD = 115200
DURATION = 180
OUTPUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'device.log')
OUTPUT = os.path.abspath(OUTPUT)

print(f"Resetting device on {PORT}...")

try:
    s = serial.Serial(PORT, BAUD, timeout=0.1)
except Exception as e:
    print(f"ERROR: {e}", file=sys.stderr)
    sys.exit(1)

# Hard reset via RTS
s.rts = True
time.sleep(0.1)
s.rts = False

print(f"Capturing for {DURATION}s...")

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
print(f"\nDone: {os.path.getsize(OUTPUT)} bytes -> {OUTPUT}")
