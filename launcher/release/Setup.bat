@echo off
rem First-time setup without the launcher window: Setup.bat <dump folder>
rem Builds pkp.exe and the shader cache from your own dump, the same way the launcher's Set up button does. Resumable: run it again to
rem continue a paused or interrupted setup. Progress is also written to setup.log and setup_status.txt in this folder.
cd /d "%~dp0"
if "%~1"=="" (
    echo usage: Setup.bat ^<dump folder^>   ^(the folder with base\ and updates\^)
    exit /b 3
)
pokopia-setup.exe setup --dump "%~1"
