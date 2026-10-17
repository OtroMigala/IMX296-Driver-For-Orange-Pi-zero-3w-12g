# Compila un módulo de sensor VIN desde Windows usando el contenedor Docker.
# Uso:  .\build\compilar.ps1 imx296_mipi        (o imx219 para validar el entorno)
#       .\build\compilar.ps1 -Rebuild imx296_mipi   (reconstruye la imagen Docker)
param(
    [Parameter(Mandatory = $true)][string]$Modulo,
    [switch]$Rebuild
)
# No 'Stop': en PowerShell 5.1 el progreso que docker escribe en stderr se volvería un error.
# Los fallos se detectan con $LASTEXITCODE.
$ErrorActionPreference = 'Continue'
$raiz = Split-Path -Parent $PSScriptRoot      # ticanor-imx296-port/
$build = Join-Path $raiz 'build'
$img = 'opi-a733-kmod:6.6.98'

$existe = docker images -q $img
if ($Rebuild -or -not $existe) {
    docker build -t $img $build
    if ($LASTEXITCODE -ne 0) { throw "Falló docker build" }
}

$fuentes = Join-Path $raiz 'fuentes\vin-8a9be72'
$driver = Join-Path $raiz 'driver'
$salida = Join-Path $build 'salida'
New-Item -ItemType Directory -Force $driver, $salida | Out-Null

docker run --rm `
    -v "${fuentes}:/fuentes:ro" `
    -v "${driver}:/driver:ro" `
    -v "${salida}:/salida" `
    $img compilar.sh $Modulo
if ($LASTEXITCODE -ne 0) { throw "Falló la compilación de $Modulo" }
