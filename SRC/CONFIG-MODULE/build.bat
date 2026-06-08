@echo off
echo Installing dependencies...
pip install flask flask-cors pymodbus pyserial cheroot pyinstaller

echo.
echo Building executable...
pyinstaller --onefile ^
    --name "ModbusConfigurator" ^
    --add-data "templates;templates" ^
    --add-data "static;static" ^
    --hidden-import serial.tools.list_ports ^
    --hidden-import serial.tools.list_ports_windows ^
    --hidden-import pymodbus.client ^
    --hidden-import cheroot.wsgi ^
    app.py

echo.
echo Done! Executable is in the dist\ folder.
echo Open http://127.0.0.1:5001 in your browser after running it.
pause
