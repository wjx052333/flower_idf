Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
. "C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1"
$ProjectDir = "C:\temp\esp32\test\livekit_mqtt_cpp\client\esp32\flower_idf\ESP32-S3-CAM-OVxxxx"
Set-Location $ProjectDir
$DevLog = Join-Path $ProjectDir "device.log"

Write-Host "=== BUILD ==="
$result = idf.py build 2>&1 | Out-String
Write-Host $result
if ($LASTEXITCODE -ne 0) {
    Write-Host "BUILD FAILED"
    exit 1
}

Write-Host "=== FLASH ==="
python -m esptool --chip esp32s3 -b 460800 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m 0x0 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin 0x11000 build\ota_data_initial.bin 0x20000 build\cam_mqtt.bin 0x7e0000 build\srmodels\srmodels.bin 2>&1 | Out-Host
if ($LASTEXITCODE -ne 0) {
    Write-Host "FLASH FAILED"
    exit 1
}

Write-Host "=== CAPTURE (120s) ==="
python -c @"
import serial, sys, time, os
PORT = 'COM9'
DURATION = 120
OUTPUT = r'$DevLog'
print(f'Capturing {PORT} for {DURATION}s...')
try:
    s = serial.Serial(PORT, 115200, timeout=0.1)
except Exception as e:
    print(f'ERROR: {e}', file=sys.stderr)
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
print(f'Done: {os.path.getsize(OUTPUT)} bytes -> {OUTPUT}')
"@
Write-Host "Device log: $DevLog"
