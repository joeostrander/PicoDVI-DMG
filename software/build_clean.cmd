@ECHO OFF

SET DRIVE_LETTER=D:
@REM SET PLATFORM=rp2040
@REM SET BOARD=pico
SET PLATFORM=rp2350
SET BOARD=pico2
SET RESOLUTION_MODE=2

cd %~dp0
rmdir /s /q build 2>nul
mkdir build
cd build
echo.
echo ===== Running CMake Configuration =====
cmake -G "MinGW Makefiles" -DPICO_COPY_TO_RAM=1 -DPICO_PLATFORM=%PLATFORM% -DPICO_BOARD=%BOARD% -DRESOLUTION_MODE=%RESOLUTION_MODE% ..
if %errorlevel% neq 0 (
    echo.
    echo *** CMAKE CONFIGURATION FAILED ***
    exit /b %errorlevel%
)

echo.
echo ===== Building Project =====
cmd /c make -j4
REM cmake --build . --target dmg
if %errorlevel% neq 0 (
    echo.
    echo *** BUILD FAILED ***
    exit /b %errorlevel%
)

echo.
echo ===== Build Successful =====
echo Binary size:
dir apps\dmg\dmg.elf | find "dmg.elf"

if exist %DRIVE_LETTER%\ (
    echo.
    echo ===== Copying UF2 to %DRIVE_LETTER%\ =====
    copy apps\dmg\dmg.uf2 %DRIVE_LETTER%\
    if %errorlevel% neq 0 (
        echo *** COPY FAILED ***
        exit /b %errorlevel%
    )
    echo Copy successful!
) else (
    echo %DRIVE_LETTER%\ drive not found, skipping copy
)

echo.
echo ===== ALL DONE =====
cd %~dp0
exit /b 0