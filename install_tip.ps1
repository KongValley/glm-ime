# glm-ime install/upgrade (run elevated)
# 策略：DLL 带版本名部署（旧文件被 TSF 锁死时互不影响），注册表指向最新
$ErrorActionPreference = 'Continue'
$LOG = "$env:LOCALAPPDATA\glm_install.log"
$INSTDIR = 'C:\Program Files (x86)\glm-ime'
$SRC = 'F:\glm-ime'
$VER = Get-Date -Format 'yyyyMMddHHmmss'
$DLL = "glm-ime-tip-$VER.dll"

"=== glm-ime install/upgrade $(Get-Date) ===" | Out-File $LOG -Append -Encoding utf8
function Log($m) { $m | Out-File $LOG -Append -Encoding utf8 }

Get-Process -Name glm-launcher -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

foreach ($d in @("$INSTDIR", "$INSTDIR\x64")) {
    New-Item -ItemType Directory -Force -Path $d | Out-Null
}
Copy-Item "$SRC\tip\build\Release\glm-ime-tip.dll" "$INSTDIR\x64\$DLL" -Force
Copy-Item "$SRC\crates\launcher\target\release\glm-launcher.exe" "$INSTDIR\" -Force
Copy-Item "$SRC\crates\engine-rs\target\release\glm-engine-rs.exe" "$INSTDIR\" -Force
Copy-Item "$SRC\crates\engine-rs\lexicon.json" "$INSTDIR\" -Force

$p = Start-Process "$env:SystemRoot\System32\regsvr32.exe" -ArgumentList "/s `"$INSTDIR\x64\$DLL`"" -Wait -PassThru
if ($p.ExitCode -eq 0) { Log "OK regsvr32 $DLL" } else { Log "FAIL regsvr32 exit=$($p.ExitCode)"; exit 1 }

New-Item -Path 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Run' -Force | Out-Null
Set-ItemProperty -Path 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'glm-launcher' -Value "$INSTDIR\glm-launcher.exe"

Start-Process "$INSTDIR\glm-launcher.exe"
Start-Sleep -Seconds 2
Log "INSTALL_DONE ($DLL)"
