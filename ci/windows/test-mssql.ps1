# Load the MSSQL test data and run msautotest/mssql
param (
    [string]$SDK = "release-1944-x64",
    [string]$Server = "localhost",
    [string]$Password = "Password12!"
)
$ErrorActionPreference = "Stop"

$ROOT = (Get-Location).Path
$SDK_BIN = "$ROOT\sdk\$SDK\bin"

$env:PATH = "$ROOT\build\Release;$SDK_BIN;$SDK_BIN\gdal\apps;" + $env:PATH
$env:PROJ_DATA = "$SDK_BIN\proj9\SHARE"
$env:GDAL_DRIVER_PATH = "$SDK_BIN\gdal\plugins"
$env:CURL_CA_BUNDLE = "$SDK_BIN\curl-ca-bundle.crt"

Write-Host "Using $((Get-Command mapserv).Source)"
mapserv -v

cd msautotest

./mssql/create_mssql_db.ps1 -Server $Server -Password $Password

Copy-Item wxs\data\epsg2 $env:PROJ_DATA

python -m pip install --disable-pip-version-check -r requirements.txt
if ($LASTEXITCODE -ne 0) { throw "pip install failed" }

cd mssql
python run_test.py
exit $LASTEXITCODE
