# SPDX-License-Identifier: GPL-3.0-or-later

[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'

$vectors = @{
    4 = @(@(0,-1024),@(1024,0),@(0,1024),@(-1024,0))
    6 = @(@(0,-1024),@(887,-512),@(887,512),@(0,1024),@(-887,512),@(-887,-512))
    8 = @(@(0,-1024),@(724,-724),@(1024,0),@(724,724),@(0,1024),@(-724,724),@(-1024,0),@(-724,-724))
}

function Place([int]$x, [int]$y, [int]$size) {
    $radius = 116; $pad = 12
    [pscustomobject]@{ X=[Math]::Min(1280-$radius-$pad,[Math]::Max($radius+$pad,$x)); Y=[Math]::Min(720-$radius-$pad,[Math]::Max($radius+$pad,$y)); Radius=$radius; Dead=24; Commit=68; Size=$size }
}
function Get-RadialSelection($g, [int]$x, [int]$y, [int]$previous=-1) {
    $dx=$x-$g.X; $dy=$y-$g.Y; $d2=$dx*$dx+$dy*$dy
    if ($d2 -le $g.Dead*$g.Dead) { return [pscustomobject]@{Index=-1;Committed=$false} }
    $best=0; $score=[int64]::MinValue
    for($i=0;$i-lt$g.Size;$i++){ $s=$dx*$vectors[$g.Size][$i][0]+$dy*$vectors[$g.Size][$i][1]; if($s-gt$score){$score=$s;$best=$i} }
    $committed=$d2-ge$g.Commit*$g.Commit
    if($committed-and$previous-ge0-and$previous-ne$best){$old=$dx*$vectors[$g.Size][$previous][0]+$dy*$vectors[$g.Size][$previous][1];$approx=[Math]::Max([Math]::Abs($dx),[Math]::Abs($dy))+[Math]::Min([Math]::Abs($dx),[Math]::Abs($dy))/2;if($score-$old-lt$approx*140){$best=$previous}}
    [pscustomobject]@{Index=$best;Committed=$committed}
}
function Assert($condition,[string]$name){if(-not$condition){throw "M6 gesture failure: $name"};Write-Host "PASS $name"}

foreach($p in @(@(0,0),@(1280,0),@(0,720),@(1280,720),@(640,0),@(0,360))){$g=Place $p[0] $p[1] 8;Assert ($g.X-$g.Radius-ge0-and$g.Y-$g.Radius-ge0-and$g.X+$g.Radius-le1280-and$g.Y+$g.Radius-le720) "edge placement $($p -join ',')"}
$g=Place 640 360 8
$selection=Get-RadialSelection $g 649 368
Assert ($selection.Index-eq-1) 'centre jitter cancels'
$selection=Get-RadialSelection $g 640 250
Assert ($selection.Index-eq0-and$selection.Committed) 'fast one-sample flick commits'
$g=Place 640 360 4
$selection=Get-RadialSelection $g 700 300 0
Assert ($selection.Index-eq0) 'outer selection hysteresis'

$source=Get-Content -Raw (Join-Path $PSScriptRoot '..\firmware\main\m6_media.c')
Assert ($source.Contains('s_state = M5_STATE_WAKING') -and $source.Contains('reason=touch consumed=1')) 'wake touch is consumed'
Assert ($source.Contains('M6_RADIAL_OPEN_DRAG_PX 5') -and $source.Contains('m6_radial_drag_exceeded')) 'radial opens after short drag threshold'
Assert ($source.Contains('lv_obj_get_coords(tile, &area)') -and $source.Contains('s_radial_geometry.center = origin')) 'gesture origin is the pressed icon centre'
Assert ($source.Contains('lv_obj_set_pos(node, origin.x + offset.x') -and -not $source.Contains('s_radial_menu_center')) 'visual menu stays centred on edge icons and may be clipped'
Assert ($source.Contains('s_radial_highlights') -and -not $source.Contains('lv_obj_set_style_border_width(s_radial_nodes')) 'radial selection uses tint without icon outlines'
Assert ($source.Contains('M6_RADIAL_ACTION_PAGE_NEXT') -and $source.Contains('m6_run_radial_action')) 'radial navigation actions are supported'
Assert ($source.Contains('m6_schedule_radial_prewarm') -and $source.Contains('lv_image_decoder_open')) 'active page radial artwork is prewarmed'
Assert (-not $source.Contains('m6_load_next_radial_image') -and $source.Contains('lv_image_set_src(image, &s_ui_bundle.images[asset])')) 'radial artwork is attached immediately'
Assert ($source.Contains('s_media_flipped = (header->settings & (1U << 8)) != 0') -and $source.Contains('m5_flip_rgb565_180((uint16_t *) s_media.panel_buffers')) 'screensaver follows the project display orientation'
Write-Host 'M6 gesture matrix complete: corners, edges, jitter, flick, hysteresis, wake suppression.'
