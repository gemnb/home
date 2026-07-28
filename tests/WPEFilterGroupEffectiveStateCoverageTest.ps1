$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$h = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'UserFilterManager.h')
$cpp = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'UserFilterManager.cpp')

function Need($text, $needle, $message) {
    if ($text -notmatch [regex]::Escape($needle)) { throw $message }
}

Need $h 'struct AuthorizedFilterState' 'UserFilterManager.h must define AuthorizedFilterState.'
Need $h 'GetAuthorizedFilters' 'UserFilterManager.h must expose GetAuthorizedFilters.'
Need $h 'GetEffectiveUserFilters' 'UserFilterManager.h must keep GetEffectiveUserFilters.'
Need $h 'UpdateUserFilterGroups' 'UserFilterManager.h must expose UpdateUserFilterGroups.'
Need $h 'GetUserFilterGroups' 'UserFilterManager.h must expose GetUserFilterGroups.'
Need $cpp 'authorizedFilterIds' 'UserFilterManager.cpp must compute authorized filter ids.'
Need $cpp 'defaultEnabledFilterIds' 'UserFilterManager.cpp must compute default enabled ids.'
Need $cpp 'std::set_intersection' 'UserFilterManager.cpp must intersect user choices with authorization.'

Write-Host 'WPE filter group effective state coverage is complete.'
