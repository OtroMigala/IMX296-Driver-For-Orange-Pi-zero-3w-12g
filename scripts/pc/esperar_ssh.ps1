param(
    [double]$Desde = 0,      # instante de referencia (segundos Unix UTC); 0 = ahora
    [int]$MaxSeg = 240,
    [int]$PrimeroCaida = 0,  # 1 = esperar primero a que el SSH deje de responder
    [string]$Destino = 'opi'   # alias o usuario@ip, con llave (BatchMode)
)
# Mide cuánto tarda en volver el SSH de la placa (ssh $Destino) después de un reinicio o un panic.
function Ahora { [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() / 1000.0 }
function Responde { & ssh -o ConnectTimeout=3 -o BatchMode=yes $Destino true 2>$null; return ($LASTEXITCODE -eq 0) }
if ($Desde -eq 0) { $Desde = Ahora }
$fin = (Ahora) + $MaxSeg
if ($PrimeroCaida -eq 1) {
    while ((Ahora) -lt $fin -and (Responde)) { Start-Sleep -Milliseconds 500 }
    "SSH caido a los {0:N1} s" -f ((Ahora) - $Desde)
}
while ((Ahora) -lt $fin) {
    if (Responde) { "SSH de vuelta a los {0:N1} s" -f ((Ahora) - $Desde); exit 0 }
    Start-Sleep -Milliseconds 1000
}
"SSH sin respuesta despues de $MaxSeg s"
exit 1
