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
    & g++ -Wall -Wextra -Wpedantic -Werror $objs -o "$Root\build/atomdb.exe" -pthread -lws2_32
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

    # Ensure the library objects (src/*.cpp except main.cpp) are built and linked into
    # the test runner. (src/main.cpp defines `main` for the atomdb executable; the test
    # runner has its own main in tests/main.cpp.)
    $needBuild = $false
    New-Item -ItemType Directory -Force -Path "$Root\build\src" | Out-Null
    # Remove any pre-existing src/main.obj (would conflict with tests/main.obj).
    $mainObj = Join-Path "$Root\build\src" "main.obj"
    if (Test-Path $mainObj) { Remove-Item -LiteralPath $mainObj }
    foreach ($src in Get-ChildItem -Path "$Root\src" -Filter "*.cpp") {
        if ($src.Name -eq "main.cpp") { continue } # not a library object
        $obj = Join-Path "$Root\build\src" ($src.BaseName + ".obj")
        if (-not (Test-Path $obj)) {
            $needBuild = $true
            break
        }
    }
    if ($needBuild) {
        foreach ($src in Get-ChildItem -Path "$Root\src" -Filter "*.cpp") {
            if ($src.Name -eq "main.cpp") { continue }
            $obj = Join-Path "$Root\build\src" ($src.BaseName + ".obj")
            Write-Host "[compile] $($src.Name)"
            & g++ -std=c++2b -Wall -Wextra -Wpedantic -Werror -I"$Root\includes" -c $src.FullName -o $obj
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        }
    }
    $srcObjs = Get-ChildItem -Path "$Root\build\src" -Filter "*.obj" | ForEach-Object { $_.FullName }

    $testObjs = Get-ChildItem -Path "$Root\build\tests" -Filter "*.obj" | ForEach-Object { $_.FullName }
    $objs = $testObjs + $srcObjs
    Write-Host "[link] test_runner"
    & g++ -Wall -Wextra -Wpedantic -Werror $objs -o "$Root\build/test_runner.exe" -pthread -lws2_32
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    & "$Root\build/test_runner.exe"
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
