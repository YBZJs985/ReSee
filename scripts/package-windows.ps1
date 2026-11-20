#requires -Version 7.0
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$QtPrefix,
    [Parameter(Mandatory)][string]$FfmpegPrefix,
    [Parameter(Mandatory)][string]$MsvcRuntime,
    [Parameter(Mandatory)][string]$Destination,
    [string]$QmlDirectory = (Join-Path $PSScriptRoot '../qml')
)

$ErrorActionPreference = 'Stop'
$destinationPath = [IO.Path]::GetFullPath($Destination)
if (Test-Path -LiteralPath $destinationPath) {
    throw "Use a new deployment directory: $destinationPath"
}

$ffmpegBin = Join-Path $FfmpegPrefix 'bin'
$ffmpeg = Join-Path $ffmpegBin 'ffmpeg.exe'
$version = (& $ffmpeg -version | Out-String)
if ($LASTEXITCODE -ne 0 -or $version -notmatch 'configuration:' -or
    $version -match '--enable-(gpl|nonfree)') {
    throw 'A matching shared LGPL FFmpeg build is required for the public package.'
}
$deployQt = Join-Path $QtPrefix 'bin/windeployqt.exe'
if (!(Test-Path -LiteralPath (Join-Path $MsvcRuntime 'vcruntime140.dll'))) {
    throw 'MsvcRuntime must point to the MSVC x64 app-local runtime directory.'
}

New-Item -ItemType Directory -Path $destinationPath | Out-Null
$appPath = Join-Path $destinationPath 'ReSee.exe'
Copy-Item -LiteralPath $Executable -Destination $appPath
$originalPath = $env:PATH
try {
    $env:PATH = "$(Join-Path $QtPrefix 'bin');$MsvcRuntime;$originalPath"
    & $deployQt --verbose 0 --release --no-translations --no-ffmpeg --no-compiler-runtime --no-opengl-sw `
        --exclude-plugins ffmpegmediaplugin --include-plugins windowsmediaplugin,qsvg `
        --qmldir $QmlDirectory --dir $destinationPath $appPath
    if ($LASTEXITCODE -ne 0) { throw 'Qt runtime deployment failed.' }
} finally {
    $env:PATH = $originalPath
}

foreach ($library in @('avcodec','avformat','avutil','avfilter','swresample','swscale')) {
    $dll = @(Get-ChildItem -LiteralPath $ffmpegBin -File -Filter "$library-*.dll")
    if ($dll.Count -ne 1) { throw "Expected one FFmpeg DLL for $library" }
    Copy-Item -LiteralPath $dll[0].FullName -Destination $destinationPath
}
Get-ChildItem -LiteralPath $MsvcRuntime -File -Filter '*.dll' | Copy-Item -Destination $destinationPath
if (!(Test-Path -LiteralPath (Join-Path $destinationPath 'platforms/qwindows.dll')) -or
    !(Test-Path -LiteralPath (Join-Path $destinationPath 'multimedia/windowsmediaplugin.dll'))) {
    throw 'The Windows Qt platform or multimedia plugin was not deployed.'
}

$notices = Join-Path $destinationPath 'licenses'
New-Item -ItemType Directory -Path $notices | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '../LICENSE') -Destination "$notices/ReSee-MIT.txt"
Copy-Item -LiteralPath (Join-Path $FfmpegPrefix 'LICENSE.txt') -Destination "$notices/FFmpeg-LGPL.txt"
$version | Set-Content -LiteralPath "$notices/FFmpeg-build.txt" -Encoding utf8
Copy-Item -LiteralPath (Join-Path $QtPrefix 'sbom') -Destination "$notices/Qt-SBOM" -Recurse
Get-Item -LiteralPath $appPath
