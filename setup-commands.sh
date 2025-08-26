# ? one time
brew install cmake ninja dfu-util ccache git wget flex bison gperf python@3.13
brew install openssl libffi
mkdir -p ~/esp
cd ~/esp
git clone -b v5.3 https://github.com/espressif/esp-idf.git
cd esp-idf
python3.13 -m venv ~/.espressif/idf5.3_py3.13_env

# ? required for new terminal session
export IDF_PYTHON_ENV_PATH=~/.espressif/idf5.3_py3.13_env
./install.sh
source export.sh

# ? one time
cd ..
git clone https://github.com/JayS0223/libpeer.git (change branch to other-server/whip)
cd libpeer
git submodule update --init --recursive
cd examples/esp32
idf.py set-target esp32s3
idf.py menuconfig
ls /dev/cu.usb*

# ? new terminal session and run command
idf.py -p /dev/cu.usbmodem1101 flash monitor

# to terminate command
ctrl + ]