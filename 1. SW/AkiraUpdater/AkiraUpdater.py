import subprocess
import sys


def check_esptool():
    try:
        subprocess.run(["esptool.py", "--version"], check=True)
    except subprocess.CalledProcessError:
        print("esptool is not installed. Installing now...")
        subprocess.run([sys.executable, "-m", "pip", "install", "esptool"], check=True)


def upload_to_specific_address(address, binary_file):
    check_esptool()
    available_addresses = [
        "0x69000",
        "0x82000",
        "0xc3000",
        "0x124000",
        "0x145000",
        "0x186000",
        "0x19f000",
        "0x1b8000",
        "0x1d1000",
        "0x292000",
        "0x313000",
        "0x35d000",
        "0x39e000",
        "0x3df000",
    ]
    command = [
        "esptool.py",
        "--chip",
        "esp32",
        "--port",
        "/dev/ttyUSB0",
        "--baud",
        "921600",
        "write_flash",
        available_addresses[address],
        binary_file,
    ]
    subprocess.run(command, check=True)


# Example usage
upload_to_specific_address(1, "my_firmware.bin")
