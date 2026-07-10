$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSCommandPath
Set-Location $Root

function Invoke-Build {
    New-Item -ItemType Directory -Force -Path "$Root\build\src" | Out-Null
    New-Item -ItemType Directory -Force -Path "$Root\build\tests" | Out-Null

    if (-not (Test-Path "$Root\src\main.cpp")) {
        Write-Error "src\main.cpp missing -- repo layout changed?"
        exit 1
    }

    foreach ($src in Get-ChildItem -Path "$Root\src" -Filter "*.cpp") {
        $obj = Join-Path "$Root\build\src" ($src.BaseName + ".obj")
        Write-Host "[compile] $($src.Name)"
        & g++ -std=c++2b -Wall -Wextra -Wpedantic -Werror -I"$Root\includes" -c $src.FullName -o $obj
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }

    Write-Host "[link] atomdb"
    $objs = Get-ChildItem -Path "$Root\build\src" -Filter "*.obj" | ForEach-Object { $_.FullName }
    & g++ -Wall -Wextra -Wpedantic -Werror $objs -o "$Root\build\atomdb.exe" -pthread
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

function Invoke-Tests {
    New-Item -ItemType Directory -Force -Path "$Root\build\tests" | Out-Null
    foreach ($src in Get-ChildItem -Path "$Root\tests" -Filter "*.test.cpp") {
        $obj = Join-Path "$Root\build\tests" ($src.BaseName + ".obj")
        Write-Host "[compile] $($src.Name)"
        & g++ -std=c++2b -Wall -Wextra -Wpedantic -Werror -I"$Root\includes" -c $src.FullName -o $obj
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }

    Write-Host "[compile] main.cpp"
    & g++ -std=c++2b -Wall -Wextra -Wpedantic -Werror -I"$Root\includes" -c "$Root\tests\main.cpp" -o "$Root\build\tests\main.obj"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    $objs = Get-ChildItem -Path "$Root\build\tests" -Filter "*.obj" | ForEach-Object { $_.FullName }
    Write-Host "[link] test_runner"
    & g++ -Wall -Wextra -Wpedantic -Werror $objs -o "$Root\build\test_runner.exe" -pthread
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    & "$Root\build\test_runner.exe"
}

function Invoke-Smoke {
    if (-not (Test-Path "$Root\build\atomdb.exe")) {
        Write-Host "build/atomdb.exe missing; building first."
        Invoke-Build
    }
    $inputFile = "$Root\build\smoke_input.txt"
    Set-Content -Path $inputFile -Encoding ascii -Value @'
INSERT users {key:1,name:nikhil,age:30}
SELECT users
EXIT
'@
    Get-Content $inputFile | & "$Root\build\atomdb.exe"
}

switch ($args[0]) {
    "build" { Invoke-Build }
    "test"  { Invoke-Tests }
    "smoke" { Invoke-Smoke }
    "clean" {
        if (Test-Path "$Root\build") { Remove-Item -Recurse -Force "$Root\build" }
        Write-Host "build/ removed."
    }
    default { Write-Host "Usage: ./build.ps1 {build|test|smoke|clean}"; exit 2 }
}
