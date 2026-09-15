#!/bin/sh
# 在 git-bash(MSYS) 里调用 ESP-IDF 的包装脚本。
#
# 为什么需要它：
#   1) 用户 profile 会强制注入 MSYSTEM=MINGW64，而 IDF 5.5 的 tools/idf.py 在
#      `if 'MSYSTEM' in os.environ:` 分支里只打印一句 "continue at your own risk"
#      就结束了 —— 它不会调用 main()，所以命令什么也不做、还返回 0。
#      必须真正从进程环境里删掉 MSYSTEM，用 shell 的 unset 不生效。
#   2) IDF 自带的 export.sh 会按 PATH 里的 python3 去推算虚拟环境名，这里直接指定
#      venv 里的 python.exe，避免用到系统 Python 3.14 而找不到 idf5.5_py3.14_env。
#
# 用法：tools/idf.sh build / tools/idf.sh -p /dev/ttyUSB0 flash monitor
exec /d/Runtime/Espressif/python_env/idf5.5_py3.11_env/Scripts/python.exe -c '
import os, subprocess, sys

IDF_PATH     = r"D:\Runtime\Espressif\frameworks\esp-idf-v5.5.5"
IDF_TOOLS    = r"D:\Runtime\Espressif"
# ESP32-C3 是 RISC-V 核，工具链是 riscv32-esp-elf；构建时 IDF 只会挑当前 target
# 对应的那一个用，所以两个都挂上不冲突。xtensa 那条留给需要回退到 ESP32-S3 的场合。
TOOLCHAIN_RV = IDF_TOOLS + r"\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin"
TOOLCHAIN_XT = IDF_TOOLS + r"\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin"
NINJA        = IDF_TOOLS + r"\tools\ninja\1.12.1"
VENV_SCRIPTS = IDF_TOOLS + r"\python_env\idf5.5_py3.11_env\Scripts"
CCACHE       = IDF_TOOLS + r"\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64"
ROM_ELFS     = IDF_TOOLS + r"\tools\esp-rom-elfs\20241011"

env = {k: v for k, v in os.environ.items() if k != "MSYSTEM"}
env["IDF_PATH"] = IDF_PATH
env["IDF_TOOLS_PATH"] = IDF_TOOLS
env["ESP_ROM_ELF_DIR"] = ROM_ELFS
env["IDF_PYTHON_ENV_PATH"] = IDF_TOOLS + r"\python_env\idf5.5_py3.11_env"
for p in (TOOLCHAIN_RV, TOOLCHAIN_XT, NINJA, VENV_SCRIPTS, CCACHE):
    env["PATH"] = p + ";" + env["PATH"]

sys.exit(subprocess.call(
    [sys.executable, IDF_PATH + r"\tools\idf.py", "-C", r"D:\Projects\ESP32\zw"] + sys.argv[1:],
    env=env,
))
' "$@"
