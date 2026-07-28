$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bridge = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'ui_bridge.cpp')
$cloud = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'CloudIntegration.cpp')
$abp = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'ABProtectIntegration.cpp')

function NeedAny($texts, $needle, $message) {
    foreach ($text in $texts) {
        if ($text -match [regex]::Escape($needle)) { return }
    }
    throw $message
}

NeedAny @($bridge) 'save_card_wpe_filter_groups' 'ui_bridge.cpp must save card filter group binding.'
NeedAny @($bridge) 'save_user_wpe_filter_groups' 'ui_bridge.cpp must save user filter group binding.'
NeedAny @($cloud, $abp, $bridge) 'LoadCardWPEFilterGroups' 'Activation/login flow must load card filter groups.'
NeedAny @($cloud, $abp, $bridge) 'SaveUserWPEFilterGroups' 'Activation/login flow must persist user inherited filter groups.'

Write-Host 'WPE filter group card binding coverage is complete.'
