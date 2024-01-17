import subprocess
import os
import sys
import tkinter as tk
from tkinter import ttk
from tkinter import filedialog
import serial.tools.list_ports


# Define the addresses and sizes
flash_data = [
    ("0x69000", "rom01.bin", 100),
    ("0x82000", "rom02.bin", 260),
    ("0xc3000", "rom03.bin", 388),
    # ... [rest of your addresses and sizes]
    ("0x3df000", "rom14.bin", 132),
]

selected_roms = {i: "" for i in range(len(flash_data))}


def ensure_packages():
    # Check for esptool and install if not present
    try:
        esptool_path = (
            subprocess.check_output(
                [sys.executable, "-c", "import esptool; print(esptool.__file__)"]
            )
            .decode()
            .strip()
        )
        print(f"esptool found at: {esptool_path}")
    except (subprocess.CalledProcessError, ImportError):
        print("esptool not found. Attempting to install...")
        subprocess.run([sys.executable, "-m", "pip", "install", "esptool"], check=True)

    # Check for tkinter and install if not present
    try:
        import tkinter
    except ImportError:
        print("tkinter is not installed. Installing now...")
        subprocess.run([sys.executable, "-m", "pip", "install", "tkinter"], check=True)


def get_esptool_path():
    try:
        esptool_script_path = (
            subprocess.check_output(
                [sys.executable, "-c", "import esptool; print(esptool.__file__)"]
            )
            .decode()
            .strip()
        )
        if os.path.exists(esptool_script_path):
            return esptool_script_path.replace("__init__.py", "esptool.py")
    except (subprocess.CalledProcessError, ImportError):
        pass

    try:
        result = subprocess.run(
            ["where" if os.name == "nt" else "which", "esptool.py"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=True,
        )
        esptool_path = result.stdout.decode().strip()
        return esptool_path
    except subprocess.CalledProcessError:
        print("Error: esptool.py path not found.")
        return None


def flash_rom_to_esp32(address, filename, size, esptool_path, port):
    command = [
        sys.executable,
        esptool_path,
        "--chip",
        "esp32",
        "--port",
        port,
        "--baud",
        "115200",
        "write_flash",
        address,
        filename,
    ]
    subprocess.run(command, check=True)


def select_file(index):
    file_path = filedialog.askopenfilename(title=f"Select ROM {index + 1} File")
    if file_path:
        selected_roms[index] = file_path


def main():
    ensure_packages()
    esptool_path = get_esptool_path()
    if not esptool_path:
        return

    root = tk.Tk()
    root.title("Akira Game Console Updater")
    root.geometry("650x460")
    root.configure(bg="#333333")

    style = ttk.Style(root)
    style.theme_use("clam")
    style.configure(
        "TButton", foreground="#FFFFFF", background="#FF5733", font=("Arial", 12)
    )
    style.configure(
        "TLabel", foreground="#FFFFFF", background="#333333", font=("Arial", 12)
    )
    style.configure("TFrame", background="#333333")
    style.map("TButton", background=[("active", "#FF8C33")])

    frame = ttk.Frame(root)
    frame.grid(row=0, column=0, padx=50, pady=50)

    ttk.Label(
        frame, text="Akira Game Console Updater", font=("Arial", 20, "bold")
    ).grid(row=0, column=0, columnspan=3, pady=20)
    ttk.Label(frame, text="Selected Chip: esp32", font=("Arial", 12)).grid(
        row=1, column=0, sticky=tk.W, pady=5
    )

    available_ports = [port.device for port in serial.tools.list_ports.comports()]
    port_var = tk.StringVar(root)
    port_var.set(available_ports[0] if available_ports else "No COM Ports Available")
    ttk.Label(frame, text="Select Port:", font=("Arial", 12)).grid(
        row=2, column=0, sticky=tk.W, pady=5
    )
    ttk.OptionMenu(frame, port_var, *available_ports).grid(
        row=2, column=1, sticky=tk.W, pady=5
    )

    for index, (address, _, size) in enumerate(flash_data):
        ttk.Label(frame, text=f"ROM {index + 1}").grid(
            row=index + 3, column=0, sticky=tk.W, pady=5
        )
        ttk.Label(frame, text=f"Address: {address} | Size: {size}KB").grid(
            row=index + 3, column=1, sticky=tk.W, pady=5
        )
        ttk.Button(
            frame,
            text=f"Select ROM {index + 1} File",
            command=lambda i=index: select_file(i),
        ).grid(row=index + 3, column=2, sticky=tk.W, pady=5)

    ttk.Button(
        frame,
        text="Flash ROMs",
        command=lambda: [
            flash_rom_to_esp32(
                flash_data[i][0],
                selected_roms[i],
                flash_data[i][2],
                esptool_path,
                port_var.get(),
            )
            for i in range(len(flash_data))
        ],
        width=20,
    ).grid(row=len(flash_data) + 3, columnspan=3, pady=20)

    root.mainloop()


if __name__ == "__main__":
    main()
