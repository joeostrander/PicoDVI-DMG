@ECHO OFF
cd %~dp0
rmdir /s /q build 2>nul
mkdir build
cd build
REM cmake -G "MinGW Makefiles" -DPICO_SDK_PATH=/path/to/sdk -DPICO_PLATFORM=rp2040 -DPICO_COPY_TO_RAM=1 ..
REM cmake -G "MinGW Makefiles" -DPICO_SDK_PATH=/path/to/sdk -DPICO_PLATFORM=rp2350 -DPICO_COPY_TO_RAM=1 ..
REM cmake --build . --target dmg
echo.
echo ===== Running CMake Configuration =====
cmake -G "MinGW Makefiles" -DPICO_COPY_TO_RAM=1 ..
if %errorlevel% neq 0 (
    echo.
    echo *** CMAKE CONFIGURATION FAILED ***
    exit /b %errorlevel%
)

echo.
echo ===== Building Project =====
cmd /c make -j4
if %errorlevel% neq 0 (
    echo.
    echo *** BUILD FAILED ***
    exit /b %errorlevel%
)

echo.
echo ===== Build Successful =====
echo Binary size:
dir apps\dmg\dmg.elf | find "dmg.elf"

if exist e:\ (
    echo.
    echo ===== Copying UF2 to E:\ =====
    copy apps\dmg\dmg.uf2 e:\
    if %errorlevel% neq 0 (
        echo *** COPY FAILED ***
        exit /b %errorlevel%
    )
    echo Copy successful!
) else (
    echo E:\ drive not found, skipping copy
)

echo.
echo ===== ALL DONE =====
exit /b 0