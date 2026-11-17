<#
.SYNOPSIS
    Installs Clipboard Voice (32-bit and 64-bit) with one SAPI voice per language.

.EXAMPLE
    .\install.ps1                 # English only
    .\install.ps1 en es           # English and Spanish
    .\install.ps1 -Languages en-GB,es-MX

.NOTES
    Asks for administrator rights (UAC) when needed. Running it again replaces
    the installed DLLs and the set of languages.
#>
[CmdletBinding()]
param(
    # Locale names: en, es, fr, es-MX, ... "en" keeps the original "Clipboard Voice" name.
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$Languages = @('en'),
    [string]$X64Dll,
    [string]$X86Dll,
    # Internal: set by the elevated copy of this script.
    [string]$LogFile
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

function Write-Log([string]$Message, [string]$Level = 'INFO') {
    $line = if ($Level) { "[$Level] $Message" } else { $Message }
    Write-Host $line
    if ($LogFile) { Add-Content -LiteralPath $LogFile -Value $line -Encoding UTF8 }
}

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal $id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Allow "en,es" or "en es" or -Languages en,es.
$Languages = @($Languages | ForEach-Object { $_ -split '[,;\s]+' } | Where-Object { $_ } |
               ForEach-Object { $_.Trim().Replace('_', '-') } | Select-Object -Unique)
if (-not $Languages) { $Languages = @('en') }

foreach ($lang in $Languages) {
    try { $ci = [Globalization.CultureInfo]::GetCultureInfo($lang) }
    catch { Write-Log "Unknown language '$lang'. Use locale names such as en, es, fr, es-MX." 'ERROR'; exit 2 }
    if ($ci.LCID -eq 4096) { Write-Log "Language '$lang' has no Windows language ID; SAPI can't use it." 'ERROR'; exit 2 }
}

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $X64Dll) { $X64Dll = Join-Path $repoRoot 'ClipboardVoice\x64\Release\ClipboardVoice.dll' }
if (-not $X86Dll) { $X86Dll = Join-Path $repoRoot 'ClipboardVoice\x86\Release\ClipboardVoice.dll' }

# ---- Elevate ----
if (-not (Test-Admin)) {
    Write-Log 'Administrator rights are needed; asking Windows for them (UAC prompt)...'
    $log = Join-Path ([IO.Path]::GetTempPath()) ("ClipboardVoice-install-{0}.log" -f [guid]::NewGuid())
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"",
                 '-Languages', ($Languages -join ','), '-X64Dll', "`"$X64Dll`"", '-X86Dll', "`"$X86Dll`"",
                 '-LogFile', "`"$log`"")
    try {
        $p = Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') -ArgumentList $argList `
                           -Verb RunAs -WindowStyle Hidden -Wait -PassThru
    } catch {
        Write-Log 'Elevation was cancelled or failed; nothing was installed.' 'ERROR'
        exit 1
    }
    if (Test-Path -LiteralPath $log) {
        Get-Content -LiteralPath $log -Encoding UTF8 | ForEach-Object { Write-Host $_ }
        Remove-Item -LiteralPath $log -ErrorAction SilentlyContinue
    }
    exit $p.ExitCode
}

# ---- Paths ----
$is64Os = [Environment]::Is64BitOperatingSystem
# A 32-bit PowerShell on 64-bit Windows sees redirected System32/Program Files; use the real ones.
$sys32 = if ($is64Os -and -not [Environment]::Is64BitProcess) { Join-Path $env:windir 'Sysnative' } else { Join-Path $env:windir 'System32' }
$installs = @()
if ($is64Os) {
    $installs += [pscustomobject]@{ Arch = 'x64'; Source = $X64Dll; Dir = (Join-Path $env:ProgramW6432 'ClipboardVoice');
                                    RegSvr = (Join-Path $sys32 'regsvr32.exe'); PowerShell = (Join-Path $sys32 'WindowsPowerShell\v1.0\powershell.exe') }
    $installs += [pscustomobject]@{ Arch = 'x86'; Source = $X86Dll; Dir = (Join-Path ${env:ProgramFiles(x86)} 'ClipboardVoice');
                                    RegSvr = (Join-Path $env:windir 'SysWOW64\regsvr32.exe'); PowerShell = (Join-Path $env:windir 'SysWOW64\WindowsPowerShell\v1.0\powershell.exe') }
} else {
    $installs += [pscustomobject]@{ Arch = 'x86'; Source = $X86Dll; Dir = (Join-Path $env:ProgramFiles 'ClipboardVoice');
                                    RegSvr = (Join-Path $sys32 'regsvr32.exe'); PowerShell = (Join-Path $sys32 'WindowsPowerShell\v1.0\powershell.exe') }
}

$langArg = $Languages -join ','
Write-Log "Installing Clipboard Voice for languages: $langArg"
$errors = 0

foreach ($i in $installs) {
    Write-Log "--- $($i.Arch) ---" ''
    if (-not (Test-Path -LiteralPath $i.Source)) {
        Write-Log "$($i.Arch) DLL not found: $($i.Source). Build Release|$($i.Arch -replace 'x86','Win32') first." 'ERROR'
        $errors++; continue
    }
    $dest = Join-Path $i.Dir 'ClipboardVoice.dll'
    try {
        New-Item -ItemType Directory -Force -Path $i.Dir | Out-Null
        # Old copies renamed by a previous install because a program had them loaded.
        Get-ChildItem -LiteralPath $i.Dir -Filter '*.old' -ErrorAction SilentlyContinue |
            Remove-Item -Force -ErrorAction SilentlyContinue
        try {
            Copy-Item -LiteralPath $i.Source -Destination $dest -Force
        } catch {
            if (-not (Test-Path -LiteralPath $dest)) { throw }
            # A running program (game, screen reader) has the DLL loaded. Windows allows
            # renaming a loaded DLL, so move it aside; that program keeps the old one
            # until it restarts.
            $aside = "$dest.$([DateTime]::Now.ToString('yyyyMMddHHmmss')).old"
            Rename-Item -LiteralPath $dest -NewName (Split-Path -Leaf $aside)
            Copy-Item -LiteralPath $i.Source -Destination $dest -Force
            Write-Log "The old $($i.Arch) DLL is in use; programs using it get the new one after they restart." 'WARN'
        }
        Unblock-File -LiteralPath $dest -ErrorAction SilentlyContinue
        Write-Log "Copied to $dest"
    } catch {
        Write-Log "Could not copy the $($i.Arch) DLL to $($i.Dir): $($_.Exception.Message)" 'ERROR'
        $errors++; continue
    }

    # /i:<languages> runs DllRegisterServer, then DllInstall which creates one voice per language.
    $p = Start-Process -FilePath $i.RegSvr -ArgumentList @('/s', "/i:$langArg", "`"$dest`"") -Wait -PassThru -WindowStyle Hidden
    if ($p.ExitCode -ne 0) {
        $why = switch ($p.ExitCode) {
            3 { 'the DLL could not be loaded (blocked by antivirus, or wrong architecture?)' }
            4 { 'registration entry point missing (old DLL?)' }
            5 { 'registering the COM class failed' }
            default { "regsvr32 exit code $($p.ExitCode) (a language may be invalid)" }
        }
        Write-Log "Registering the $($i.Arch) DLL failed: $why." 'ERROR'
        $errors++; continue
    }
    Write-Log "Registered $($i.Arch) COM class and voices."
}

# ---- Verify: list and load every Clipboard Voice from each architecture ----
$verify = @'
$ErrorActionPreference = 'Stop'
$v = New-Object -ComObject SAPI.SpVoice
$found = 0
foreach ($t in $v.GetVoices()) {
    if ($t.Id -notmatch '\\ClipboardVoice(_[^\\]*)?$') { continue }
    $found++
    try {
        $v.Voice = $t
        $v.AudioOutputStream = New-Object -ComObject SAPI.SpMemoryStream
        # Silence only: runs the engine without touching the clipboard.
        [void]$v.Speak('<silence msec="10"/>', 8)
        "OK   " + $t.GetDescription() + "  [Language " + $t.GetAttribute('Language') + "]"
    } catch {
        "FAIL " + $t.GetDescription() + ": " + $_.Exception.Message
    }
}
if ($found -eq 0) { "FAIL no Clipboard Voice found" }
'@
$encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($verify))
Write-Log '--- Verification ---' ''
foreach ($i in $installs) {
    if (-not (Test-Path -LiteralPath $i.PowerShell)) { continue }
    $out = & $i.PowerShell -NoProfile -NonInteractive -EncodedCommand $encoded 2>&1
    foreach ($line in $out) {
        $text = "$line"
        if ($text -like 'FAIL*') { $errors++; Write-Log "$($i.Arch): $text" 'ERROR' } else { Write-Log "$($i.Arch): $text" }
    }
    $expected = $Languages.Count
    $ok = @($out | Where-Object { "$_" -like 'OK*' }).Count
    if ($ok -lt $expected) {
        Write-Log "$($i.Arch): expected $expected voice(s), found $ok working." 'ERROR'
        $errors++
    }
}

Write-Log '' ''
if ($errors) {
    Write-Log "Installation finished with $errors error(s); see above." 'FAIL'
    exit 1
}
Write-Log "Clipboard Voice installed for: $langArg" 'SUCCESS'
Write-Log 'Test it with: powershell -ExecutionPolicy Bypass -File test\say.ps1 "Hello"' ''
exit 0
