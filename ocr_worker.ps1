[Console]::InputEncoding = [System.Text.Encoding]::UTF8
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

Add-Type -AssemblyName System.Runtime.WindowsRuntime
[Windows.Media.Ocr.OcrEngine, Windows.Foundation, ContentType=WindowsRuntime] | Out-Null
[Windows.Graphics.Imaging.BitmapDecoder, Windows.Foundation, ContentType=WindowsRuntime] | Out-Null
[Windows.Storage.StorageFile, Windows.Foundation, ContentType=WindowsRuntime] | Out-Null

$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and
    $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
})[0]

function Await($WinRtTask, $ResultType) {
    $asTask = $asTaskGeneric.MakeGenericMethod($ResultType)
    $netTask = $asTask.Invoke($null, @($WinRtTask))
    $netTask.Wait(-1) | Out-Null
    $netTask.Result
}

$engine = $null
try {
    $enLang = [Windows.Globalization.Language]::new("en-US")
    if ([Windows.Media.Ocr.OcrEngine]::IsLanguageSupported($enLang)) {
        $engine = [Windows.Media.Ocr.OcrEngine]::TryCreateFromLanguage($enLang)
    }
} catch {}
if ($null -eq $engine) {
    $engine = [Windows.Media.Ocr.OcrEngine]::TryCreateFromUserProfileLanguages()
}
Write-Output "READY"
[Console]::Out.Flush()

while ($true) {
    $line = [Console]::In.ReadLine()
    if ($null -eq $line) { break }
    $path = $line.Trim()
    if ($path -eq "EXIT") { break }
    if ([string]::IsNullOrWhiteSpace($path) -or -not (Test-Path $path)) {
        Write-Output "ERR:"
        [Console]::Out.Flush()
        continue
    }

    try {
        $file = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($path)) ([Windows.Storage.StorageFile])
        $stream = Await ($file.OpenAsync([Windows.Storage.FileAccessMode]::Read)) ([Windows.Storage.Streams.IRandomAccessStream])
        $decoder = Await ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($stream)) ([Windows.Graphics.Imaging.BitmapDecoder])
        $bitmap = Await ($decoder.GetSoftwareBitmapAsync()) ([Windows.Graphics.Imaging.SoftwareBitmap])
        if ($null -eq $engine) {
            Write-Output "OK:"
        } else {
            $result = Await ($engine.RecognizeAsync($bitmap)) ([Windows.Media.Ocr.OcrResult])
            $sortedLines = $result.Lines | ForEach-Object {
                $firstWord = $_.Words | Select-Object -First 1
                [PSCustomObject]@{
                    Y = if ($firstWord) { [double]$firstWord.BoundingRect.Y } else { 999999.0 }
                    X = if ($firstWord) { [double]$firstWord.BoundingRect.X } else { 999999.0 }
                    Text = $_.Text
                }
            } | Sort-Object Y, X
            $joined = ($sortedLines | ForEach-Object { $_.Text }) -join "`n"
            $txt = $joined -replace "`r", "" -replace "`n", "\n"
            Write-Output ("OK:" + $txt)
        }
        $stream.Dispose()
    } catch {
        Write-Output "ERR:"
    }
    [Console]::Out.Flush()
}
