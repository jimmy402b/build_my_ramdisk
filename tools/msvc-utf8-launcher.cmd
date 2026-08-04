@echo off
chcp 65001 >nul
%*
set "MYRD_COMPILER_EXIT=%ERRORLEVEL%"
exit /b %MYRD_COMPILER_EXIT%
