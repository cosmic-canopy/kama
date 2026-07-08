# kama installer:  irm https://kama-lang.org/install.ps1 | iex
#
# Detects arch and whether a C compiler is present, then installs the matching
# release into %USERPROFILE%\.kama. Re-running updates in place.
#   -NoStd / $env:KAMA_NO_STD=1   skip the bundled standard library
#   $env:KAMA_VERSION=vX.Y.Z      install a specific release (default: latest)
#   $env:KAMA_HOME=<dir>          install prefix (default: %USERPROFILE%\.kama)
param([switch]$NoStd)
$ErrorActionPreference = 'Stop'
$Repo   = 'cosmic-canopy/kama'
$Prefix = if ($env:KAMA_HOME) { $env:KAMA_HOME } else { "$env:USERPROFILE\.kama" }
$Ver    = if ($env:KAMA_VERSION) { $env:KAMA_VERSION } else { 'latest' }
$arch   = if ([Environment]::Is64BitOperatingSystem) { 'x64' } else { throw 'kama-install: unsupported arch' }

$hasCC  = @('cl','clang','gcc') | Where-Object { Get-Command $_ -ErrorAction SilentlyContinue }
$flavor = if ($hasCC) { '' } else { '-bundled' }
Write-Host "kama-install: $(if ($hasCC) {'C compiler found - slim build'} else {'no compiler - self-contained build (bundled zig cc)'})"

if ($Ver -eq 'latest') { $Ver = (Invoke-RestMethod "https://api.github.com/repos/$Repo/releases/latest").tag_name }
$asset = "kama-windows-$arch$flavor-$Ver.tar.gz"
$url   = "https://github.com/$Repo/releases/download/$Ver/$asset"

$tmp = New-Item -ItemType Directory -Path ([IO.Path]::GetTempPath()) -Name ([Guid]::NewGuid())
Write-Host "kama-install: downloading $asset"
Invoke-RestMethod $url -OutFile "$tmp\$asset"
try {
  $want = (Invoke-RestMethod "$url.sha256").Split(' ')[0]
  $got  = (Get-FileHash "$tmp\$asset" -Algorithm SHA256).Hash
  if ($want -and $got -ne $want) { throw 'checksum mismatch' }
  Write-Host 'kama-install: checksum ok'
} catch { Write-Host 'kama-install: warning: checksum not verified' }

New-Item -ItemType Directory -Force -Path $Prefix | Out-Null
$bin = "$Prefix\bin"
# Windows can't overwrite a running .exe — move an existing one aside first (self-update).
if (Test-Path "$bin\kama.exe") { Move-Item "$bin\kama.exe" "$bin\kama.exe.old" -Force -EA SilentlyContinue }
tar -xzf "$tmp\$asset" -C $Prefix --strip-components=1        # tar ships with Windows 10+
Remove-Item "$bin\kama.exe.old" -Force -EA SilentlyContinue
if ($NoStd -or $env:KAMA_NO_STD) { Remove-Item -Recurse -Force "$Prefix\lib\kama" -EA SilentlyContinue; Write-Host 'kama-install: skipped stdlib' }

Write-Host "kama-install: installed kama $Ver to $bin"
if (-not ($env:Path -split ';' | Where-Object { $_ -eq $bin })) {
  [Environment]::SetEnvironmentVariable('Path', "$bin;$([Environment]::GetEnvironmentVariable('Path','User'))", 'User')
  Write-Host 'kama-install: added to your user PATH (restart the shell)'
}
& "$bin\kama.exe" --version
