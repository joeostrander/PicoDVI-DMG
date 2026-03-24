@echo off
setlocal

SET DRIVE_LETTER=D:
if "%PLATFORM%"=="" SET PLATFORM=rp2350
if "%BOARD%"=="" SET BOARD=pico2
if "%RESOLUTION_MODE%"=="" SET RESOLUTION_MODE=2

cd %~dp0

if exist build\apps\dmg\CMakeFiles\dmg.dir\main.c.obj del /q build\apps\dmg\CMakeFiles\dmg.dir\main.c.obj
if exist build\apps\dmg\CMakeFiles\dmg.dir\main.c.obj.d del /q build\apps\dmg\CMakeFiles\dmg.dir\main.c.obj.d
if exist build\elf2uf2\CMakeFiles\elf2uf2.dir\main.cpp.obj del /q build\elf2uf2\CMakeFiles\elf2uf2.dir\main.cpp.obj
if exist build\elf2uf2\CMakeFiles\elf2uf2.dir\main.cpp.obj.d del /q build\elf2uf2\CMakeFiles\elf2uf2.dir\main.cpp.obj.d
if exist build\pioasm\CMakeFiles\pioasm.dir\main.cpp.obj del /q build\pioasm\CMakeFiles\pioasm.dir\main.cpp.obj
if exist build\pioasm\CMakeFiles\pioasm.dir\main.cpp.obj.d del /q build\pioasm\CMakeFiles\pioasm.dir\main.cpp.obj.d

if not exist build\CMakeCache.txt (
    echo.
    echo ===== CMake Cache Missing: Configuring Build Folder =====
    cmake -S . -B build -G "MinGW Makefiles" -DPICO_COPY_TO_RAM=1 -DPICO_PLATFORM=%PLATFORM% -DPICO_BOARD=%BOARD% -DRESOLUTION_MODE=%RESOLUTION_MODE% -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    if %errorlevel% neq 0 (
        echo.
        echo *** CMAKE CONFIGURATION FAILED ***
        goto :build_fail
    )
)


echo.
echo ===== Building Project =====
cmake --build build --parallel 4
if %errorlevel% neq 0 (
    echo.
    echo *** BUILD FAILED ***
    goto :build_fail
)

echo.
echo ===== Build Successful =====
if exist build\apps\dmg\dmg.elf (
    echo Binary size:
    dir build\apps\dmg\dmg.elf | find "dmg.elf"
)

if exist %DRIVE_LETTER%\ (
    echo.
    echo ===== Copying UF2 to %DRIVE_LETTER%\ =====
    copy /y build\apps\dmg\dmg.uf2 %DRIVE_LETTER%\ >nul
    if %errorlevel% neq 0 (
        echo *** COPY FAILED ***
        goto :build_fail
    )
    echo Copy successful!
) else (
    echo %DRIVE_LETTER%\ drive not found, skipping copy
)

goto :eof

:build_fail
echo Build or copy failed.
exit /b 1