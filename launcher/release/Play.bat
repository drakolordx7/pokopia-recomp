@echo off
rem Starts @SHORT@ from this folder without the launcher. Every path (exefs, romfs, shader cache) comes from @INI@ next to
rem @EXE@, relative paths resolved against this folder: run the launcher's setup once (it writes @INI@ and builds
rem shader_cache\spirv). Saves stay in .\save, as with the launcher.
cd /d "%~dp0"
if not defined NXRT_FS_SAVE_DIR set "NXRT_FS_SAVE_DIR=%~dp0save"
start "" @EXE@ --log @LOG@
