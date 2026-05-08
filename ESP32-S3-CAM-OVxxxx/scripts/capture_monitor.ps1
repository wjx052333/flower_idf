Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
. "C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1"
$ProjectDir = "C:\temp\esp32\test\livekit_mqtt_cpp\client\esp32\flower_idf\ESP32-S3-CAM-OVxxxx"
$DevLog = Join-Path $ProjectDir "device.log"
Set-Location $ProjectDir

Write-Host "Starting monitor (120s)..."

$job = Start-Job -ScriptBlock {
    param($dir)
    Set-Location $dir
    & idf.py -p COM9 monitor 2>&1 | ForEach-Object { $_ }
} -ArgumentList $ProjectDir

Start-Sleep -Seconds 120
Stop-Job $job
$output = Receive-Job $job
Remove-Job $job

$output | Out-File -FilePath $DevLog -Encoding UTF8
Write-Host "Device log saved to $DevLog"
Write-Host "--- Device Log ---"
$output
