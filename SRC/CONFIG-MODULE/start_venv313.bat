@echo on
set VENV_NAME=%USERPROFILE%\AppData\Local\Programs\Python\Python313\venv313
cmd /k call "%VENV_NAME%\Scripts\activate.bat"

REM We use this only if in the same folder with the venv311
REM cmd /k call "%~dp0venv311test\Scripts\activate"