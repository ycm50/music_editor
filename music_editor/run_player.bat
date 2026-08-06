@echo off
set PATH=%~dp0build\player;A:\msys2\ucrt64\bin;%PATH%
"%~dp0build\player\player.exe" %*
