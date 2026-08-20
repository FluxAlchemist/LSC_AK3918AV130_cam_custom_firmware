# gen_version.ps1 — called by Makefile before compiling version.c
# Increments build_number.txt and writes version_info.h.

$BuildNumFile = 'build_number.txt'

if (-not (Test-Path $BuildNumFile)) { Set-Content $BuildNumFile '0' }
$num = [int](Get-Content $BuildNumFile) + 1
Set-Content $BuildNumFile "$num"

$git = try { (git describe --always --dirty=-dirty 2>$null).Trim() } catch { 'unknown' }
if ([string]::IsNullOrEmpty($git)) { $git = 'unknown' }

$ts = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'

@"
#pragma once
#define BUILD_NUMBER $num
#define BUILD_TIMESTAMP "$ts"
#define GIT_HASH "$git"
"@ | Set-Content 'version_info.h'

Write-Host "[mk] build #$num  git:$git  $ts"
