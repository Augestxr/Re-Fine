# discord_fake.ps1 - Prueba de Discord Rich Presence sin Discord: abre \\.\pipe\discord-ipc-0 como lo
# haria el cliente, contesta READY al handshake y a cada comando, y escribe cada trama recibida en -Out.
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\discord_fake.ps1 -Out work\run\discord.txt [-Seconds 200]
#       (antes de lanzar el juego con refine.ini discord_app_id = <cualquier numero>)
# No usar con Discord abierto: el nombre de la tuberia seria el mismo.

param(
    [Parameter(Mandatory)][string]$Out,
    [int]$Seconds = 200
)

$ErrorActionPreference = 'Stop'
$end = (Get-Date).AddSeconds($Seconds)
"" | Set-Content -Encoding utf8 $Out

function Read-Exact($s, [int]$n) {
    $b = New-Object byte[] $n; $got = 0
    while ($got -lt $n) {
        $t = $s.ReadAsync($b, $got, $n - $got)
        while (-not $t.IsCompleted) { if ((Get-Date) -gt $end) { return $null }; Start-Sleep -Milliseconds 50 }
        if ($t.Result -le 0) { return $null }
        $got += $t.Result
    }
    return $b
}
function Send-Frame($s, [int]$op, [string]$json) {
    $j = [Text.Encoding]::UTF8.GetBytes($json)
    $h = [BitConverter]::GetBytes($op) + [BitConverter]::GetBytes($j.Length)
    $s.Write([byte[]]($h + $j), 0, 8 + $j.Length); $s.Flush()
}

while ((Get-Date) -lt $end) {
    $s = New-Object IO.Pipes.NamedPipeServerStream('discord-ipc-0', [IO.Pipes.PipeDirection]::InOut, 1,
        [IO.Pipes.PipeTransmissionMode]::Byte, [IO.Pipes.PipeOptions]::Asynchronous)
    $w = $s.WaitForConnectionAsync()
    while (-not $w.IsCompleted -and (Get-Date) -lt $end) { Start-Sleep -Milliseconds 100 }
    if (-not $s.IsConnected) { $s.Dispose(); break }
    Add-Content -Encoding utf8 $Out "connected $(Get-Date -Format HH:mm:ss.fff)"
    while ($s.IsConnected) {
        $h = Read-Exact $s 8
        if (-not $h) { break }
        $op = [BitConverter]::ToInt32($h, 0); $len = [BitConverter]::ToInt32($h, 4)
        $body = Read-Exact $s $len
        if (-not $body) { break }
        $json = [Text.Encoding]::UTF8.GetString($body)
        Add-Content -Encoding utf8 $Out "op=$op $json"
        if ($op -eq 0) { Send-Frame $s 1 '{"cmd":"DISPATCH","evt":"READY","data":{"v":1}}' }
        elseif ($op -eq 1) { Send-Frame $s 1 '{"cmd":"SET_ACTIVITY","evt":null,"data":{}}' }
        elseif ($op -eq 2) { break }
    }
    Add-Content -Encoding utf8 $Out "closed $(Get-Date -Format HH:mm:ss.fff)"
    $s.Dispose()
}
