$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$h = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'UserFilterWebServer.h')
$cpp = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'UserFilterWebServer.cpp')

function Need($text, $needle, $message) {
    if ($text -notmatch [regex]::Escape($needle)) { throw $message }
}

Need $h 'AuthorizedWebFilter' 'UserFilterWebServer.h must define AuthorizedWebFilter.'
Need $h 'SetAuthorizedFilterListGetter' 'UserFilterWebServer.h must expose authorized filter getter.'
Need $cpp '"collectorFilters"' 'UserFilterWebServer.cpp must return collectorFilters.'
Need $cpp '"heartbeatFilters"' 'UserFilterWebServer.cpp must return heartbeatFilters.'
Need $cpp 'authorizedFilterIds' 'UserFilterWebServer.cpp must enforce authorization.'
Need $cpp 'defaultEnabled' 'UserFilterWebServer.cpp must return default enabled state.'
Need $cpp 'data-auth-filter-id' 'HTML must render authorized filter ids for saving.'

Write-Host 'User filter web authorization coverage is complete.'
