param([string]$Text = "Hello from Clipboard Voice. If this text is spoken by another voice, clipboard voice did not work. If this text is in your clipboard, it worked!")

$v = New-Object -ComObject SAPI.SpVoice
$voices = $v.GetVoices("Name=Clipboard Voice")
if ($voices.Count -gt 0) {
    $v.Voice = $voices.Item(0)
} else {
    Write-Host "Clipboard Voice not found; using default voice."
}

$v.Speak($Text) | Out-Null
Write-Host "Spoke. Check clipboard for the text."

