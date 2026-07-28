$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$h = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'DatabaseManager.h')
$cpp = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'DatabaseManager.cpp')

function Assert-Contains($text, $pattern, $message) {
    if ($text -notmatch [regex]::Escape($pattern)) {
        throw $message
    }
}

Assert-Contains $h 'struct WPEFilterGroupRecord' 'DatabaseManager.h must define WPEFilterGroupRecord.'
Assert-Contains $h 'struct WPEFilterGroupItemRecord' 'DatabaseManager.h must define WPEFilterGroupItemRecord.'
Assert-Contains $h 'CreateWPEFilterGroupTables' 'DatabaseManager.h must declare CreateWPEFilterGroupTables.'
Assert-Contains $h 'SaveWPEFilterGroup' 'DatabaseManager.h must declare SaveWPEFilterGroup.'
Assert-Contains $h 'LoadWPEFilterGroups' 'DatabaseManager.h must declare LoadWPEFilterGroups.'
Assert-Contains $h 'SaveUserWPEFilterGroups' 'DatabaseManager.h must declare SaveUserWPEFilterGroups.'
Assert-Contains $h 'LoadUserWPEFilterGroups' 'DatabaseManager.h must declare LoadUserWPEFilterGroups.'
Assert-Contains $h 'SaveCardWPEFilterGroups' 'DatabaseManager.h must declare SaveCardWPEFilterGroups.'
Assert-Contains $h 'LoadCardWPEFilterGroups' 'DatabaseManager.h must declare LoadCardWPEFilterGroups.'

Assert-Contains $cpp 'CREATE TABLE IF NOT EXISTS wpe_filter_groups' 'DatabaseManager.cpp must create wpe_filter_groups.'
Assert-Contains $cpp 'CREATE TABLE IF NOT EXISTS wpe_filter_group_items' 'DatabaseManager.cpp must create wpe_filter_group_items.'
Assert-Contains $cpp 'CREATE TABLE IF NOT EXISTS user_wpe_filter_groups' 'DatabaseManager.cpp must create user_wpe_filter_groups.'
Assert-Contains $cpp 'CREATE TABLE IF NOT EXISTS card_wpe_filter_groups' 'DatabaseManager.cpp must create card_wpe_filter_groups.'
Assert-Contains $cpp 'default_enabled INTEGER NOT NULL DEFAULT 0' 'Group item table must persist default enabled state.'
Assert-Contains $cpp 'CreateWPEFilterGroupTables()' 'InitializeConfig must call CreateWPEFilterGroupTables.'

Write-Host 'WPE filter group database coverage is complete.'
