@echo off
setlocal

REM Clear any MSYS environment variables
set MSYSTEM=
set MSYSCON=
set MSYS=
set TERM=

REM Set up ESP-IDF paths
set "IDF_PATH=C:\Users\Dhruv\.esp-tools\esp-idf"
set "IDF_TOOLS_PATH=C:\Users\Dhruv\.espressif"
set "PYTHON_PATH=C:\Users\Dhruv\.espressif\python_env\idf5.4_py3.11_env\Scripts"

REM Set up PATH with ESP-IDF tools
set "PATH=%PYTHON_PATH%;%IDF_PATH%\tools;%PATH%"

REM Get the export vars from IDF tools
for /f "usebackq tokens=1,2 delims==" %%a in (`%PYTHON_PATH%\python.exe %IDF_PATH%\tools\idf_tools.py export --format key-value 2^>nul`) do (
    if "%%a"=="PATH" (
        set "PATH=%%b"
    ) else if not "%%a"=="" (
        set "%%a=%%b"
    )
)

REM Change to project directory
cd /d "C:\Users\Dhruv\Documents\Projects\LinuxCompatibilityLayer"

REM Execute the command
echo Running: %PYTHON_PATH%\python.exe %IDF_PATH%\tools\idf.py %*
%PYTHON_PATH%\python.exe %IDF_PATH%\tools\idf.py %*

endlocal
