<#
.SYNOPSIS
    Removes Clipboard Voice and Sapi2Prism: all their SAPI voices, the COM registration and the installed DLLs.
    Asks for administrator rights (UAC) when needed.
#>
[CmdletBinding()]
param(
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

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal $id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Log 'Administrator rights are needed; asking Windows for them (UAC prompt)...'
    $log = Join-Path ([IO.Path]::GetTempPath()) ("ClipboardVoice-uninstall-{0}.log" -f [guid]::NewGuid())
    try {
        $p = Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') -Verb RunAs -WindowStyle Hidden -Wait -PassThru `
                           -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-LogFile', "`"$log`"")
    } catch {
        Write-Log 'Elevation was cancelled or failed; nothing was removed.' 'ERROR'
        exit 1
    }
    if (Test-Path -LiteralPath $log) {
        Get-Content -LiteralPath $log -Encoding UTF8 | ForEach-Object { Write-Host $_ }
        Remove-Item -LiteralPath $log -ErrorAction SilentlyContinue
    }
    exit $p.ExitCode
}

$is64Os = [Environment]::Is64BitOperatingSystem
$sys32 = if ($is64Os -and -not [Environment]::Is64BitProcess) { Join-Path $env:windir 'Sysnative' } else { Join-Path $env:windir 'System32' }
$installs = @()
if ($is64Os) {
    $installs += [pscustomobject]@{ Arch = 'x64'; Dir = (Join-Path $env:ProgramW6432 'ClipboardVoice'); RegSvr = (Join-Path $sys32 'regsvr32.exe'); View = [Microsoft.Win32.RegistryView]::Registry64 }
    $installs += [pscustomobject]@{ Arch = 'x86'; Dir = (Join-Path ${env:ProgramFiles(x86)} 'ClipboardVoice'); RegSvr = (Join-Path $env:windir 'SysWOW64\regsvr32.exe'); View = [Microsoft.Win32.RegistryView]::Registry32 }
} else {
    $installs += [pscustomobject]@{ Arch = 'x86'; Dir = (Join-Path $env:ProgramFiles 'ClipboardVoice'); RegSvr = (Join-Path $sys32 'regsvr32.exe'); View = [Microsoft.Win32.RegistryView]::Default }
}

$errors = 0
foreach ($i in $installs) {
    $dll = Join-Path $i.Dir 'ClipboardVoice.dll'
    if (Test-Path -LiteralPath $dll) {
        $p = Start-Process -FilePath $i.RegSvr -ArgumentList @('/s', '/u', "`"$dll`"") -Wait -PassThru -WindowStyle Hidden
        if ($p.ExitCode -eq 0) { Write-Log "Unregistered the $($i.Arch) DLL." }
        else { Write-Log "regsvr32 /u ($($i.Arch)) returned $($p.ExitCode); cleaning the registry directly." 'WARN' }
    }

    # Registry cleanup, also covers a DLL that was deleted (e.g. by antivirus) before uninstalling.
    $hklm = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, $i.View)
    try {
        $tokens = $hklm.OpenSubKey('SOFTWARE\Microsoft\Speech\Voices\Tokens', $true)
        if ($tokens) {
            foreach ($name in $tokens.GetSubKeyNames()) {
                if ($name -match '^(ClipboardVoice|Sapi2Prism)(_.*)?$') { $tokens.DeleteSubKeyTree($name, $false); Write-Log "Removed voice token $name ($($i.Arch) view)." }
            }
            $tokens.Close()
        }
        $classes = $hklm.OpenSubKey('SOFTWARE\Classes', $true)
        if ($classes) {
            $classes.DeleteSubKeyTree('CLSID\{F4ABF2E3-9F21-4B8C-B410-4A0A9E5C5BD3}', $false)
            $classes.DeleteSubKeyTree('ClipboardVoice.Engine', $false)
            $classes.DeleteSubKeyTree('CLSID\{8D3B6C1E-2F47-4A5D-9B0E-6C1F2A7D4E95}', $false)
            $classes.DeleteSubKeyTree('Sapi2Prism.Engine', $false)
            $classes.Close()
        }
    } catch {
        Write-Log "Registry cleanup ($($i.Arch)) failed: $($_.Exception.Message)" 'ERROR'; $errors++
    } finally { $hklm.Close() }

    if (Test-Path -LiteralPath $i.Dir) {
        try {
            Remove-Item -LiteralPath $i.Dir -Recurse -Force
            Write-Log "Removed $($i.Dir)."
        } catch {
            Write-Log "Could not remove $($i.Dir) (a running program may still have the DLL loaded). Delete it after restarting that program." 'WARN'
        }
    }
}

if ($errors) { Write-Log "Uninstall finished with $errors error(s)." 'FAIL'; exit 1 }
Write-Log 'Clipboard Voice uninstalled.' 'SUCCESS'
exit 0
