$ErrorActionPreference = "Continue"

$env:NETWORK_CFG = "dev"
$env:RUN = "test"

Push-Location build

& ..\ci\actions\windows\run.bat
if (${LastExitCode} -ne 0) {
    throw "Failed to Pass Test ${env:RUN}"
}

Pop-Location