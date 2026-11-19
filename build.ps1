$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $root 'src'
$build = Join-Path $root 'build'
$dist = Join-Path $root 'dist'
$third = Join-Path $root 'third_party\minhook-master'

$toolchain = Get-ChildItem (Join-Path $root '..\toolchain') -Directory -Filter 'llvm-mingw-*' |
    Sort-Object Name -Descending | Select-Object -First 1
if (-not $toolchain) {
    throw 'llvm-mingw not found under ..\toolchain'
}

$bin = Join-Path $toolchain.FullName 'bin'
$clang = Join-Path $bin 'clang.exe'
$clangxx = Join-Path $bin 'clang++.exe'
$windres = Join-Path $bin 'windres.exe'

New-Item -ItemType Directory -Force $build, $dist | Out-Null
$tmp = Join-Path $build 'tmp'
New-Item -ItemType Directory -Force $tmp | Out-Null
$env:TMP = $tmp
$env:TEMP = $tmp
$dllObj = Join-Path $build 'obj'
New-Item -ItemType Directory -Force $dllObj | Out-Null

$common = @(
    '-std=c++20', '-O2', '-DNDEBUG', '-DUNICODE', '-D_UNICODE',
    '-DWIN32_LEAN_AND_MEAN', '-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00',
    '-DNTDDI_VERSION=0x0A000000', '-finput-charset=UTF-8',
    "-I$src", "-I$build", "-I$third\include", "-I$third\src"
)

$cCommon = @(
    '-O2', '-DNDEBUG', '-DUNICODE', '-D_UNICODE', '-DWIN32_LEAN_AND_MEAN',
    '-D_WIN32_WINNT=0x0A00', "-I$third\include", "-I$third\src"
)

Write-Host 'Building MinHook...'
$minhookObjs = @()
foreach ($file in @('buffer.c', 'hook.c', 'trampoline.c', 'hde\hde64.c')) {
    $name = [IO.Path]::GetFileNameWithoutExtension($file)
    $obj = Join-Path $dllObj ("mh_$name.o")
    & $clang @cCommon -c (Join-Path $third "src\$file") -o $obj
    if ($LASTEXITCODE -ne 0) { throw "MinHook compile failed: $file" }
    $minhookObjs += $obj
}

Write-Host 'Building mod DLL...'
$modObj = Join-Path $dllObj 'mod.o'
$runtimeObj = Join-Path $dllObj 'runtime.o'
& $clangxx @common -c (Join-Path $src 'mod.cpp') -o $modObj
if ($LASTEXITCODE -ne 0) { throw 'mod.cpp compile failed' }
& $clangxx @common -c (Join-Path $src 'windhawk_runtime.cpp') -o $runtimeObj
if ($LASTEXITCODE -ne 0) { throw 'windhawk_runtime.cpp compile failed' }

$dll = Join-Path $build 'SlickWindowArrangement.dll'
& $clangxx -shared -static -static-libgcc -o $dll `
    $modObj $runtimeObj @minhookObjs `
    -lcomctl32 -ldwmapi -lshcore -luser32 -lshell32 -lole32 -ladvapi32 `
    -lpsapi -lshlwapi -luuid
if ($LASTEXITCODE -ne 0) { throw 'DLL link failed' }

Write-Host 'Embedding DLL...'
$embedHeader = Join-Path $build 'embedded_dll.h'
& python (Join-Path $root 'tools\embed.py') $dll $embedHeader
if ($LASTEXITCODE -ne 0) { throw 'embedding failed' }

Write-Host 'Building resources...'
$resourceObj = Join-Path $build 'app.res.o'
& $windres "-I$src" -i (Join-Path $src 'app.rc') -o $resourceObj --codepage=65001
if ($LASTEXITCODE -ne 0) { throw 'resource compile failed' }

Write-Host 'Building launcher EXE...'
$exe = Join-Path $dist 'SlickWindowArrangement.exe'
& $clangxx @common '-DSLICK_EMBEDDED' -mwindows `
    (Join-Path $src 'injector.cpp') $resourceObj -o $exe `
    -static -static-libgcc -lcomctl32 -lshell32 -luser32 -ladvapi32 -lole32 -lshlwapi -luuid
if ($LASTEXITCODE -ne 0) { throw 'EXE link failed' }

Write-Host ''
Write-Host "Built:"
Get-Item $dll, $exe | ForEach-Object { Write-Host ("  {0} ({1:N0} bytes)" -f $_.FullName, $_.Length) }
