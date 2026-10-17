@echo off
chcp 65001 >nul
cd /d "%~dp0"
if not exist "%~dp0Ingnomia.exe" (
	echo Не найден Ingnomia.exe. Распакуйте архив целиком и запустите этот файл из его папки.
	pause
	exit /b 1
)
start "" "%~dp0Ingnomia.exe"
