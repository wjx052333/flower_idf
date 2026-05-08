$ProjectDir = "C:\temp\esp32\test\livekit_mqtt_cpp\client\esp32\flower_idf\ESP32-S3-CAM-OVxxxx"
$BuildLog = Join-Path $ProjectDir "build.log"
$DevLog   = Join-Path $ProjectDir "device.log"

. "C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1"
Set-Location $ProjectDir

Write-Host "=== BUILD ==="
idf.py build > $BuildLog 2>&1
Get-Content $BuildLog
if ($LASTEXITCODE -ne 0) {
    Write-Host "BUILD FAILED (exit=$LASTEXITCODE)"
    exit 1
}

Write-Host "=== FLASH ==="
idf.py flash 2>&1 | ForEach-Object { $_ | Out-File -FilePath $BuildLog -Append; Write-Host $_ }
if ($LASTEXITCODE -ne 0) {
    Write-Host "FLASH FAILED (exit=$LASTEXITCODE)"
    exit 1
}

Write-Host "=== MONITOR (90s) ==="
$proc = Start-Process -FilePath "idf.py" -ArgumentList "monitor" -NoNewWindow -RedirectStandardOutput $DevLog -PassThru
Start-Sleep -Seconds 90
if (!$proc.HasExited) { Stop-Process $proc.Id -Force }
Write-Host "Device log: $DevLog"
Get-Content $DevLog
