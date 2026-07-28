$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bridge = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'ui_bridge.cpp')
$app = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'web\app.js')

function Need($text, $needle, $message) {
    if ($text -notmatch [regex]::Escape($needle)) { throw $message }
}

Need $bridge 'get_wpe_filter_groups' 'ui_bridge.cpp must handle get_wpe_filter_groups.'
Need $bridge 'save_wpe_filter_group' 'ui_bridge.cpp must handle save_wpe_filter_group.'
Need $bridge 'delete_wpe_filter_group' 'ui_bridge.cpp must handle delete_wpe_filter_group.'
Need $app 'renderWebUserStateSettings' 'web/app.js must render Web用户态设置 submenu.'
Need $app 'renderApiAddressSettings' 'web/app.js must render API地址 submenu.'
Need $app 'renderWpeFilterGroups' 'web/app.js must render WPE filter groups.'
Need $app 'saveWpeFilterGroup' 'web/app.js must save WPE filter groups.'
Need $app 'defaultEnabled' 'web/app.js must expose default enabled per group filter item.'

Write-Host 'WPE filter group admin UI coverage is complete.'
