$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$h = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'UserFilterWebServer.h')
$cpp = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'UserFilterWebServer.cpp')
$pc = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'PacketCollector.cpp')

function Need($text, $needle, $message) {
    if ($text -notmatch [regex]::Escape($needle)) { throw $message }
}

Need $h 'SetUserGameInfoGetter' 'UserFilterWebServer.h must expose game info getter.'
Need $h 'SetUserPoolClearer' 'UserFilterWebServer.h must expose pool clearer.'
Need $cpp '/api/clear-pool' 'UserFilterWebServer.cpp must add clear pool endpoint.'
Need $cpp '"gameId"' 'UserFilterWebServer.cpp must return gameId.'
Need $cpp '"poolCount"' 'UserFilterWebServer.cpp must return poolCount.'
Need $cpp 'HandleClearPool' 'UserFilterWebServer.cpp must implement HandleClearPool.'
Need $pc 'ClearByUsername' 'PacketCollector.cpp must wire ClearByUsername to web callback.'

Write-Host 'User filter web pool control coverage is complete.'
