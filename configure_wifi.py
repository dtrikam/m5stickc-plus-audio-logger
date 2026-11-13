#!/usr/bin/env python3
"""
Helper utility to interact with the M5StickC Plus over USB Serial (/dev/cu.usbserial-*):
- View on-device SPIFFS flash logs (/device.log) recorded while unplugged
- Check live status
- Scan or set 2.4GHz Wi-Fi credentials in ESP32 NVS flash
- Provision Cloud Server Host, Port, and Device Auth Token in ESP32 NVS flash

Usage:
  uv run --with pyserial python3 configure_wifi.py logs
  uv run --with pyserial python3 configure_wifi.py status
  uv run --with pyserial python3 configure_wifi.py scan
  uv run --with pyserial python3 configure_wifi.py set "MyWiFiSSID" ["MyWiFiPassword"]
  uv run --with pyserial python3 configure_wifi.py cloud <host> <port> [token]
"""
import getpass
import glob
import sys
import time
import serial


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbserial-*"))
    if not ports:
        raise RuntimeError("No /dev/cu.usbserial-* device found. Is the M5StickC Plus plugged in?")
    return ports[0]


def main():
    port = find_port()
    cmd = sys.argv[1] if len(sys.argv) > 1 else "status"

    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.5
    s.dtr = False
    s.rts = False
    s.open()
    time.sleep(0.3)

    # If opening the port triggered a reset, wait for setup() to complete
    t_wait = time.time()
    booting = False
    while s.in_waiting and time.time() - t_wait < 8.0:
        line = s.readline().decode("utf-8", errors="replace").strip()
        if "ets Jul" in line or "BOOT:" in line:
            booting = True
        if booting and ("WIFI_CONNECTED" in line or "WIFI_OFFLINE" in line):
            break
    if booting:
        time.sleep(0.3)
    s.reset_input_buffer()

    if cmd == "logs":
        s.write(b"LOGS\n")
        t0 = time.time()
        while time.time() - t0 < 8.0:
            line = s.readline().decode("utf-8", errors="replace").rstrip("\r\n")
            if line:
                print(line)
                if "=== END ON-DEVICE FLASH LOG ===" in line:
                    break
    elif cmd == "scan":
        s.write(b"SCAN\n")
        t0 = time.time()
        while time.time() - t0 < 6.0:
            line = s.readline().decode("utf-8", errors="replace").strip()
            if line:
                print(line)
                if "[WIFI_SCAN] Done." in line:
                    break
    elif cmd == "set" and len(sys.argv) >= 3:
        ssid = sys.argv[2]
        pw = sys.argv[3] if len(sys.argv) >= 4 else getpass.getpass(f"Wi-Fi password for '{ssid}': ")
        s.write(f"WIFI:{ssid}:{pw}\n".encode("utf-8"))
        t0 = time.time()
        while time.time() - t0 < 14.0:
            line = s.readline().decode("utf-8", errors="replace").strip()
            if line:
                print(line)
                if "[WIFI_OK]" in line or "[WIFI_ERR]" in line:
                    break
    elif cmd == "cloud" and len(sys.argv) >= 4:
        host = sys.argv[2]
        port_num = sys.argv[3]
        token = sys.argv[4] if len(sys.argv) >= 5 else getpass.getpass("Device Auth Token: ")
        s.write(f"CLOUD:{host}:{port_num}:{token}\n".encode("utf-8"))
        t0 = time.time()
        while time.time() - t0 < 8.0:
            line = s.readline().decode("utf-8", errors="replace").strip()
            if line:
                print(line)
                if "[CLOUD_OK]" in line or "[CLOUD_WARN]" in line or "[CLOUD_ERR]" in line:
                    break
    elif cmd == "status":
        s.write(b"STATUS\n")
        t0 = time.time()
        while time.time() - t0 < 2.0:
            line = s.readline().decode("utf-8", errors="replace").strip()
            if line:
                print(line)
    else:
        print(__doc__)

    s.close()


if __name__ == "__main__":
    main()
