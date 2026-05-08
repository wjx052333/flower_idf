"""Reset ESP32 via esptool run, then capture serial logs."""
import serial, sys, time, os, subprocess

PORT = 'COM9'
BAUD = 115200
DURATION = 120
OUTPUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'device.log')
OUTPUT = os.path.abspath(OUTPUT)

print(f"Resetting ESP32 via esptool run on {PORT}...")
result = subprocess.run(
    ['esptool.py', '--chip', 'esp32s3', '--port', PORT, 'run'],
    capture_output=True, text=True
)
print(result.stdout)
if result.returncode != 0:
    print(f"esptool stderr: {result.stderr}")

time.sleep(1)

print(f"Capturing for {DURATION}s -> {OUTPUT}")

try:
    s = serial.Serial(PORT, BAUD, timeout=0.1)
except Exception as e:
    print(f"ERROR: {e}", file=sys.stderr)
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
print(f"\nDone: {os.path.getsize(OUTPUT)} bytes -> {OUTPUT}")
