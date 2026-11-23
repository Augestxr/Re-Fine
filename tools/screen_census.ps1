# screen_census.ps1 - 6d: censo por pantalla. Misma entrada a dos fps, capturas en los mismos
# instantes de tiempo real desde cada pulsacion, y tools/screen_census.py dice que pantalla va
# por frames.
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\screen_census.ps1 -Plan menus -Tag c1
#           [-Fps 30,120] [-Only 120] [-Exe otro.exe] [-Env "VAR=x"]
#
# Planes (guiones ciegos por tiempo desde el arranque, perfil test):
#   menus   pelicula, titulo, partidas, carga, Home, Diva Room          (~85 s)
#   song    Play, seleccion de cancion, carga, PV (Cat Food, PAD_BOT; por segundo de cancion,
#           0x12CBE48), avisos de desbloqueo y resultados (~372 s)
#   home2   tienda (sale con Yes en "Return to Home Menu?"), Other y su primer apartado (~126 s)
#   edit    Edit desde el Home                                           (~96 s)
#   pause   menu de pausa en la PV: abrir (START a 20 s de cancion), bajar, subir, cerrar con
#           Continuar, y otra vez con START (~180 s). Pasos +<s>: tras la pulsacion anterior
# Cada paso es <s>:<teclas>:<n>:<ms>:<nombre> del diagnostico SHOTS (src/main.cpp): la pulsacion
# y las capturas van en el mismo reloj. Los pasos con n=0 solo navegan. Salida en work\census\<Tag>_<fps>\
# y la tabla del censo al final. Corridas cortas: nada de barridos.

param(
    [Parameter(Mandatory)][ValidateSet('menus', 'song', 'home2', 'edit', 'pause')][string]$Plan,
    [Parameter(Mandatory)][string]$Tag,
    [int[]]$Fps = @(30, 120),
    [int[]]$Only = @(),          # repetir solo estos fps; los demas se reutilizan
    [string]$Exe = '',
    [string]$Env = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$N = '28:100'   # 28 capturas cada 100 ms: 2,8 s, cabe entre pulsaciones separadas 3 s
$boot = '38:s:0:0:-;43:n:0:0:-;47:n:0:0:-;50:n:0:0:-'
$plans = @{
    menus = @{ Close = 84; Bot = $false; Start = $true; Steps =
        "12:.:${N}:peli1;20:.:${N}:peli2;34.5:.:${N}:titulo_idle;38:s:${N}:titulo;43:n:${N}:partidas;" +
        "47:n:${N}:confirmar;50:n:${N}:carga;54:.:${N}:home_entra;60:n:${N}:aviso_tv;64:d:${N}:cursor;" +
        "68:n:${N}:droom;72:.:${N}:droom2;76:.:${N}:droom3;80:.:${N}:droom4" }
    song = @{ Close = 372; Bot = $true; Start = $true; Steps =
        "${boot};60:n:0:0:-;64:.:${N}:home_idle;72:n:${N}:jugar;77:n:${N}:selcancion;84:n:0:0:-;" +
        "87:n:0:0:-;90:n:0:0:-;93:n:${N}:sel_lista;96:n:${N}:detalle;99:n:${N}:confirma;" +
        "102:n:${N}:carga_c;105:n:${N}:carga_c2;108:n:${N}:tarjeta;111:n:0:0:-;114:n:0:0:-;" +
        "f12CBE48@8:.:${N}:pv_8;f12CBE48@40:.:${N}:pv_40;f12CBE48@95:.:${N}:pv_95;326:.:${N}:aviso_desbl;" +
        "340:n:${N}:aviso_pag;346:n:${N}:aviso_pag2;352:n:${N}:res_rango;358:n:${N}:vuelta" }
    home2 = @{ Close = 126; Bot = $false; Start = $true; Steps =
        "${boot};60:n:0:0:-;64:d:0:0:-;66:d:${N}:cursor2;70:n:${N}:tienda;74:.:${N}:tienda2;" +
        "78:n:${N}:tienda3;82:n:${N}:tienda4;86:b:${N}:tienda_sale;90:b:${N}:sale2;94:n:${N}:home_vuelta;" +
        "98:d:0:0:-;100:r:${N}:other_cursor;104:n:${N}:other_entra;108:.:${N}:other2;112:d:${N}:other3;" +
        "116:n:${N}:other_sub;120:.:${N}:other_sub2" }
    pause = @{ Close = 180; Bot = $true; Start = $true; Steps =
        "${boot};60:n:0:0:-;64:.:0:0:-;72:n:0:0:-;77:n:0:0:-;84:n:0:0:-;87:n:0:0:-;90:n:0:0:-;" +
        "93:n:0:0:-;96:n:0:0:-;99:n:0:0:-;102:n:0:0:-;105:n:0:0:-;108:n:0:0:-;111:n:0:0:-;114:n:0:0:-;" +
        "f12CBE48@20:s:${N}:pausa_abre;+3:.:${N}:pausa_quieta;+3:d:${N}:pausa_baja;+3:u:${N}:pausa_sube;" +
        "+3:n:${N}:pausa_cierra;f12CBE48@32:s:${N}:pausa2_abre;+3:s:${N}:pausa2_start" }
    edit = @{ Close = 96; Bot = $false; Start = $true; Steps =
        "${boot};60:n:0:0:-;64:d:0:0:-;66:d:0:0:-;68:d:${N}:edit_cursor;72:n:${N}:edit_entra;" +
        "76:.:${N}:edit2;80:.:${N}:edit3;84:n:${N}:edit4;88:.:${N}:edit5" }
}
$p = $plans[$Plan]
$runFps = if ($Only.Count) { $Only } else { $Fps }
foreach ($f in $runFps) {
    $d = Join-Path $root "work\census\${Tag}_$f"
    if (Test-Path $d) { Remove-Item $d -Recurse -Force }
    New-Item -ItemType Directory -Force $d | Out-Null
    $envs = "PS3_VERBOSE=0,REFINE_FPS_TARGET=$f,PAD_KEYS=0:.:1,SHOTS=$d|$($p.Steps)"
    if ($p.Bot) { $envs += ',PAD_BOT=0' }
    if ($Env) { $envs += ",$Env" }
    if ($p.Start) { $env:PAD_SCRIPT = '30:0x0008' }
    $a = @{ Timeout = $p.Close + 8; CloseAt = $p.Close; Tag = "${Tag}_$f"; Env = $envs }
    if ($Exe) { $a.Exe = $Exe }
    & (Join-Path $PSScriptRoot 'run.ps1') @a
    Remove-Item env:PAD_SCRIPT -ErrorAction SilentlyContinue
    $log = Join-Path $root "work\run\${Tag}_$f.err.log"
    Select-String $log -Pattern '^\[SHOTS\] (\S+) (\d+) \+([\d.]+) s' | ForEach-Object {
        "$($_.Matches[0].Groups[1].Value) $($_.Matches[0].Groups[2].Value) $($_.Matches[0].Groups[3].Value)"
    } | Set-Content (Join-Path $d 'shots.txt')
    $late = Select-String $log -Pattern '^\[SHOTS\] \S+ 0 ' | Measure-Object | Select-Object -ExpandProperty Count
    Write-Host "   $f fps: $((Get-Content (Join-Path $d 'shots.txt')).Count) capturas, $late series"
}
$a30 = Join-Path $root "work\census\${Tag}_$($Fps[0])"
foreach ($f in $Fps[1..($Fps.Count - 1)]) {
    Write-Host "== censo $Plan, $($Fps[0]) frente a $f"
    python -I (Join-Path $PSScriptRoot 'screen_census.py') $a30 (Join-Path $root "work\census\${Tag}_$f")
}
