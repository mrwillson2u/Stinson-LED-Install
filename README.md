# Stinson-LED-Install


## Setup

1. To make sure you can properly load the environment variables, install python-dotenv for platformIO:
    ```
    ~/.platformio/penv/bin/pip install python-dotenv
    ```

1. Copy `template.env` and name it `.env`. Then edit it accordingly.
    ```
    cp template.env .env
    vim .env
    ```



## Compile the binary
1. run
    ```
    pio run -e esp32doit-devkit-v1
    ```
1. The binary will be located at `<project directory>/.pio/build/esp32doit-devkit-v1/firmware.bin`.
1. From the root of your project directory, copy `firmware.bin` to the raspberry pi zero:
    ```
    scp .pio/build/esp32doit-devkit-v1/firmware.bin <user>@<hostname>
    ```
1. Use esptoool.py to upload `firmware.bin` to the esp32
    ```
    esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 460800 write_flash -z 0x1000 firmware.bin
    ```
## 