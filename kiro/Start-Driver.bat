@echo off
sc create KiroReset binPath= "%~dp0src\driver\KiroDriver.sys" type= kernel start= demand
sc start KiroReset
pause
