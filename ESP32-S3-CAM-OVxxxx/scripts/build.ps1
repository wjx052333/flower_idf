# ESP-IDF build script for flower_idf
$ProjectDir = "C:\temp\esp32\test\livekit_mqtt_cpp\client\esp32\flower_idf\ESP32-S3-CAM-OVxxxx"
$LogFile = Join-Path $ProjectDir "build.log"

# Load ESP-IDF environment
. "C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1"

Set-Location $ProjectDir

Write-Host "Building $ProjectDir ..."

idf.py build 2>&1 | Tee-Object -FilePath $LogFile -Encoding UTF8

Write-Host ""
Write-Host "Build exit code: $LASTEXITCODE"
Write-Host "Log: $LogFile"
