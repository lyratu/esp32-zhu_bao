@echo off
rem Usage: monitor.bat COM3
set MSYSTEM=
set "ESP_IDF_VERSION=6.1"
set "IDF_PATH=D:\esp\v6.1\esp-idf"
set "IDF_TOOLS_PATH=D:\Espressif\tools"
set "IDF_PYTHON_ENV_PATH=D:\Espressif\tools\python\v6.1\venv"
set "ESP_ROM_ELF_DIR=D:\Espressif\tools\esp-rom-elfs\20241011\"
set "IDF_CCACHE_ENABLE=1"
set "PY=D:\Espressif\tools\python\v6.1\venv\Scripts\python.exe"
set "PATH=D:\Espressif\tools\python\v6.1\venv\Scripts;D:\Espressif\tools\cmake\4.0.3\bin;D:\Espressif\tools\ninja\1.12.1;D:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64;D:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;D:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin;D:\Espressif\tools\esp32ulp-elf\2.38_20240113\esp32ulp-elf\bin;D:\Espressif\tools\riscv32-esp-elf-gdb\17.1_20260402\riscv32-esp-elf-gdb\bin;D:\Espressif\tools\xtensa-esp-elf-gdb\17.1_20260402\xtensa-esp-elf-gdb\bin;D:\Espressif\tools\git\cmd;%PATH%"
cd /d "%~dp0.."
if "%1"=="" (
    "%PY%" "%IDF_PATH%\tools\idf.py" monitor
) else (
    "%PY%" "%IDF_PATH%\tools\idf.py" -p %1 monitor
)
