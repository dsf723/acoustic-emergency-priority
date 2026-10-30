@echo off
chcp 65001 >nul
python -I "%~dp0tools\collect_session.py"
pause
