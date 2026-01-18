@echo off

SET DRIVE_LETTER=E:

cd %~dp0
del build\apps\dmg\CMakeFiles\dmg.dir\main.c.obj
del build\apps\dmg\CMakeFiles\dmg.dir\main.c.obj.d
del build\elf2uf2\CMakeFiles\elf2uf2.dir\main.cpp.obj
del build\elf2uf2\CMakeFiles\elf2uf2.dir\main.cpp.obj.d
del build\pioasm\CMakeFiles\pioasm.dir\main.cpp.obj
del build\pioasm\CMakeFiles\pioasm.dir\main.cpp.obj.d


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
if exist apps\dmg\dmg.elf (
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