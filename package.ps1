# glm-ime 完整打包：构建（engine-rs + launcher + TIP x64/x86）→ 测试 → zip 发行包
# 用法：powershell -File F:\glm-ime\package.ps1
$ErrorActionPreference = 'Stop'
$ROOT = 'F:\glm-ime'
$CMAKE = "$env:ProgramFiles (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$CMAKE = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$STAMP = Get-Date -Format 'yyyyMMdd-HHmm'
$OUT = "$ROOT\dist"

Write-Host "=== [1/5] build engines ==="
cargo build --release --manifest-path "$ROOT\crates\engine-rs\Cargo.toml"
cargo build --release --manifest-path "$ROOT\crates\launcher\Cargo.toml"

Write-Host "=== [2/5] build TIP x64 + x86 ==="
& $CMAKE --build "$ROOT\tip\build" --config Release --target glm-ime-tip
& $CMAKE --build "$ROOT\tip\build-win32" --config Release --target glm-ime-tip

Write-Host "=== [3/5] tests ==="
python "$ROOT\tools\compare.py"
python "$ROOT\tools\test_m3.py"
python "$ROOT\tools\test_m1.py"

Write-Host "=== [4/5] stage ==="
$stage = "$OUT\stage"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path "$stage\x64", "$stage\x86" | Out-Null
Copy-Item "$ROOT\tip\build\Release\glm-ime-tip.dll" "$stage\x64\"
Copy-Item "$ROOT\tip\build-win32\Release\glm-ime-tip.dll" "$stage\x86\"
Copy-Item "$ROOT\crates\launcher\target\release\glm-launcher.exe" "$stage\"
Copy-Item "$ROOT\crates\engine-rs\target\release\glm-engine-rs.exe" "$stage\"
Copy-Item "$ROOT\crates\engine-rs\lexicon.json" "$stage\"
Copy-Item "$ROOT\install_tip.ps1" "$stage\"

Write-Host "=== [5/5] zip ==="
$zip = "$OUT\glm-ime-$STAMP.zip"
Compress-Archive -Path "$stage\*" -DestinationPath $zip -Force
Remove-Item $stage -Recurse -Force
Write-Host "PACKAGE: $zip"
