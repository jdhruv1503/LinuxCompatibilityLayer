@echo off
REM Build script for guest ELF applications
REM Usage: build_guest_app.bat <app_name>
REM Example: build_guest_app.bat hello_world

setlocal enabledelayedexpansion

REM Configuration
set PROJECT_DIR=%~dp0..
set APPS_DIR=%PROJECT_DIR%\apps
set BUILD_DIR=%PROJECT_DIR%\build\guest_apps
set DATA_DIR=%PROJECT_DIR%\data

REM Hardcoded path to Xtensa tools (modify if your installation differs)
set XTENSA_BIN=C:\Users\Dhruv\.espressif\tools\xtensa-esp-elf\esp-14.2.0_20241119\xtensa-esp-elf\bin
set XTENSA_GCC=%XTENSA_BIN%\xtensa-esp32-elf-gcc.exe
set XTENSA_LD=%XTENSA_BIN%\xtensa-esp32-elf-ld.exe
set XTENSA_STRIP=%XTENSA_BIN%\xtensa-esp32-elf-strip.exe

REM Check if app name provided
if "%~1"=="" (
    echo Usage: %~nx0 ^<app_name^>
    echo Available apps:
    for /D %%d in (%APPS_DIR%\*) do (
        echo   - %%~nxd
    )
    exit /b 1
)

set APP_NAME=%~1
set APP_SRC=%APPS_DIR%\%APP_NAME%

REM Check if app exists
if not exist "%APP_SRC%" (
    echo Error: App not found: %APP_SRC%
    exit /b 1
)

REM Check for compiler
if not exist "%XTENSA_GCC%" (
    echo Error: Xtensa GCC not found at %XTENSA_GCC%
    echo Please update the XTENSA_BIN path in this script
    exit /b 1
)

REM Create build directory
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

echo.
echo Building guest app: %APP_NAME%
echo   Source: %APP_SRC%
echo   Output: %BUILD_DIR%\%APP_NAME%.elf
echo   Compiler: %XTENSA_GCC%
echo.

REM Compiler flags for guest ELF (matching elf_loader component requirements)
REM Based on elf_loader.cmake from Espressif component
set CFLAGS=-mlongcalls -fno-common -ffunction-sections -fdata-sections -fPIC -Os
set LDFLAGS=-nostartfiles -nostdlib -fPIC -shared -fdata-sections -ffunction-sections -Wl,--gc-sections -Wl,-z,now

REM Compile
echo Compiling %APP_NAME%/main.c...
"%XTENSA_GCC%" %CFLAGS% -c "%APP_SRC%\main.c" -o "%BUILD_DIR%\%APP_NAME%.o"
if errorlevel 1 (
    echo Error: Compilation failed
    exit /b 1
)

REM Link using GCC with app_main as entry point (ESP-IDF convention)
echo Linking %APP_NAME%.elf...
"%XTENSA_GCC%" %LDFLAGS% -e app_main "%BUILD_DIR%\%APP_NAME%.o" -o "%BUILD_DIR%\%APP_NAME%.elf"
if errorlevel 1 (
    echo Error: Linking failed
    exit /b 1
)

REM Strip unnecessary sections (matching elf_loader.cmake)
echo Stripping unnecessary sections...
if exist "%XTENSA_STRIP%" (
    "%XTENSA_STRIP%" --strip-unneeded --remove-section=.comment --remove-section=.got.loc --remove-section=.dynamic --remove-section=.xt.lit --remove-section=.xt.prop --remove-section=.xtensa.info "%BUILD_DIR%\%APP_NAME%.elf" 2>nul
)

REM Copy to data folder for LittleFS
echo Copying to data folder...
if not exist "%DATA_DIR%" mkdir "%DATA_DIR%"
copy /Y "%BUILD_DIR%\%APP_NAME%.elf" "%DATA_DIR%\%APP_NAME%.elf" >nul

REM Show file info
echo.
echo Build successful!
echo   ELF file: %BUILD_DIR%\%APP_NAME%.elf
for %%A in ("%BUILD_DIR%\%APP_NAME%.elf") do echo   Size: %%~zA bytes

echo.
echo The ELF has been copied to the data folder.
echo Rebuild the firmware with 'idf.py build' to include it in the flash image.

endlocal
