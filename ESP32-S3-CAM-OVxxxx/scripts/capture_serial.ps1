Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
. "C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1"
$ProjectDir = "C:\temp\esp32\test\livekit_mqtt_cpp\client\esp32\flower_idf\ESP32-S3-CAM-OVxxxx"
$DevLog = Join-Path $ProjectDir "device.log"
Set-Location $ProjectDir

Write-Host "Capturing device output from COM9 (120s)..."

python -c "
import serial, sys, time
try:
    s = serial.Serial('COM9', 115200, timeout=1)
    start = time.time()
    with open(r'$DevLog', 'wb') as f:
        while time.time() - start < 120:
            data = s.read(4096)
            if data:
                f.write(data)
                sys.stdout.buffer.write(data)
                sys.stdout.flush()
            else:
                time.sleep(0.05)
    s.close()
    print('Done')
except Exception as e:
    print(f'Error: {e}', file=sys.stderr)
" 2>&1

Write-Host "Device log: $DevLog"
