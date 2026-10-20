param(
    [Parameter(Mandatory = $true)][string]$Log,   # archivo de salida (se agrega al final)
    [string]$Ctl = $PSScriptRoot,                # carpeta de control: serie.stop / serie.enviar
    [int]$MaxMin = 180,
    [string]$Puerto = 'COM3'           # adaptador USB-serie (CP2102), 115200 baudios
)
# Graba la consola serie (115200 baudios) con la hora UTC de llegada de cada línea.
# Para detener: crear <Ctl>\serie.stop. Para enviar texto: escribirlo en <Ctl>\serie.enviar.
$stop = Join-Path $Ctl 'serie.stop'
$send = Join-Path $Ctl 'serie.enviar'
Remove-Item $stop, $send -ErrorAction SilentlyContinue
$p = New-Object System.IO.Ports.SerialPort $Puerto, 115200, 'None', 8, 'One'
$p.DtrEnable = $false
$p.RtsEnable = $false
$p.Encoding = [System.Text.Encoding]::GetEncoding(28591)
$p.Open()
$w = New-Object System.IO.StreamWriter($Log, $true, (New-Object System.Text.UTF8Encoding($false)))
$w.AutoFlush = $true
function Linea([string]$s) { $w.WriteLine(('[{0:HH:mm:ss.fff}] {1}' -f (Get-Date).ToUniversalTime(), $s)) }
Linea "=== inicio de grabacion (UTC) ==="
$buf = ''
$ultimo = Get-Date
$fin = (Get-Date).AddMinutes($MaxMin)
try {
    while ((Get-Date) -lt $fin -and -not (Test-Path $stop)) {
        $t = $p.ReadExisting()
        if ($t) {
            $buf += $t
            $ultimo = Get-Date
            while (($i = $buf.IndexOf("`n")) -ge 0) {
                Linea $buf.Substring(0, $i).TrimEnd("`r")
                $buf = $buf.Substring($i + 1)
            }
        } elseif ($buf -and ((Get-Date) - $ultimo).TotalSeconds -gt 1) {
            Linea $buf.TrimEnd("`r")   # linea sin fin (por ejemplo, un prompt)
            $buf = ''
        }
        if (Test-Path $send) {
            $txt = [System.IO.File]::ReadAllText($send)
            Remove-Item $send
            $p.Write($txt)
            Linea (">>> enviado: " + ($txt -replace "`r", '\r' -replace "`n", '\n'))
        }
        Start-Sleep -Milliseconds 50
    }
} finally {
    if ($buf) { Linea $buf }
    Linea "=== fin de grabacion ==="
    $w.Close()
    $p.Close()
}
