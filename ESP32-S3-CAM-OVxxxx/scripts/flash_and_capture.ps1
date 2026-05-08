Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
. "C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1"
$ProjectDir = "C:\temp\esp32\test\livekit_mqtt_cpp\client\esp32\flower_idf\ESP32-S3-CAM-OVxxxx"
Set-Location $ProjectDir

Write-Host "=== FLASH ==="
$flashCmd = "python -m esptool --chip esp32s3 -b 460800 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m 0x0 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin 0x11000 build\ota_data_initial.bin 0x20000 build\cam_mqtt.bin 0x7e0000 build\srmodels\srmodels.bin"
Invoke-Expression $flashCmd
if ($LASTEXITCODE -ne 0) {
    Write-Host "FLASH FAILED (exit=$LASTEXITCODE)"
    exit 1
}
Write-Host "Flash OK"

Write-Host "=== CAPTURE (90s) ==="
$DevLog = Join-Path $ProjectDir "device.log"
python -c @"
import serial, sys, time, os
PORT = 'COM9'
DURATION = 90
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
