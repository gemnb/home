$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$wpeH = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'WPEFilter.h')
$wpeCpp = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'WPEFilter.cpp')
$bridge = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'ui_bridge.cpp')
$app = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'web\app.js')
$webServer = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'UserFilterWebServer.cpp')

function Need($text, $needle, $message) {
    if ($text -notmatch [regex]::Escape($needle)) { throw $message }
}

Need $wpeH 'webDisplayName' 'WPEFilter.h must define webDisplayName.'
Need $wpeCpp 'item["webDisplayName"]' 'WPEFilter.cpp must export webDisplayName.'
Need $wpeCpp 'f.webDisplayName' 'WPEFilter.cpp must import webDisplayName.'
Need $bridge 'webDisplayName' 'ui_bridge.cpp must expose webDisplayName to WebView.'
Need $app 'wpeWebDisplayNameList' 'web/app.js must render Web display name settings.'
Need $app 'saveWpeWebDisplayNames' 'web/app.js must save Web display names.'
Need $webServer 'webDisplayName.empty() ? filter.name : filter.webDisplayName' 'UserFilterWebServer flow must use webDisplayName fallback.'

Write-Host 'WPE filter web display name coverage is complete.'
