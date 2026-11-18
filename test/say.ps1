param(
    [string]$Text = "Hello from Clipboard Voice. If this text is spoken by another voice, clipboard voice did not work. If this text is in your clipboard, it worked!",
    # Exact voice name, e.g. "Clipboard Voice (Spanish)".
    [string]$Voice = "Clipboard Voice (English)"
)

$v = New-Object -ComObject SAPI.SpVoice
$voices = $v.GetVoices("Name=$Voice")
if ($voices.Count -gt 0) {
    $v.Voice = $voices.Item(0)
} else {
    Write-Host "$Voice not found; using the default voice. Installed Clipboard Voice and Sapi2Prism voices:"
    foreach ($t in $v.GetVoices()) { if ($t.Id -match '\\(ClipboardVoice|Sapi2Prism)') { Write-Host "  $($t.GetDescription())" } }
}

$v.Speak($Text) | Out-Null
Write-Host "Spoke. Check clipboard for the text."
