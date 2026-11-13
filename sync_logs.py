#!/usr/bin/env python3
"""
Utility to monitor live logs & sync records between:
1. Local M5StickC Plus USB Serial (/dev/cu.usbserial-*) -> ./logs/device_serial.log
2. Your Cloud VM -> ./logs/
   - Downloads audio_brain.log, status JSON snapshot, and optionally backups SQLite memory.db.

Environment configuration (or local .env):
  M5_VM_URL=http://YOUR_VM_IP:8765
  M5_GCLOUD_HOST=your-vm-name           (optional, for memory.db SCP backup)
  M5_GCLOUD_ZONE=us-central1-a          (optional)
  M5_GCLOUD_PROJECT=your-gcp-project    (optional)
  M5_REMOTE_DB_PATH=/path/to/memory.db  (optional)

Usage:
  python3 sync_logs.py vm        # Pull latest VM logs, transcripts & SQLite DB backup to local logs/ folder
  python3 sync_logs.py serial    # Stream & record live USB serial logs from M5StickC Plus
"""
import datetime
import glob
import json
import os
import subprocess
import sys
import urllib.request

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
LOGS_DIR = os.path.join(BASE_DIR, "logs")
os.makedirs(LOGS_DIR, exist_ok=True)


def load_local_env():
    env_path = os.path.join(BASE_DIR, ".env")
    if os.path.isfile(env_path):
        with open(env_path, "r", encoding="utf-8") as f:
            for raw in f:
                line = raw.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                os.environ.setdefault(k.strip(), v.strip().strip('"').strip("'"))


load_local_env()
VM_URL = os.environ.get("M5_VM_URL", "").rstrip("/")


def sync_vm():
    if not VM_URL:
        print("[!] Set M5_VM_URL (e.g. export M5_VM_URL=http://YOUR_VM_IP:8765 or add to .env) first.")
        sys.exit(1)

    print(f"[*] Fetching live status and logs from {VM_URL} ...")
    with urllib.request.urlopen(f"{VM_URL}/api/status", timeout=10) as r:
        status_data = json.loads(r.read().decode("utf-8"))
    with urllib.request.urlopen(f"{VM_URL}/api/logs", timeout=10) as r:
        logs_data = json.loads(r.read().decode("utf-8"))

    vm_log_local = os.path.join(LOGS_DIR, "vm_audio_brain.log")
    with open(vm_log_local, "w", encoding="utf-8") as f:
        f.write("\n".join(logs_data.get("lines", [])) + "\n")

    snapshot_local = os.path.join(LOGS_DIR, "vm_status_snapshot.json")
    with open(snapshot_local, "w", encoding="utf-8") as f:
        json.dump(status_data, f, indent=2)

    print(f"[✓] Saved VM logs to: {vm_log_local}")
    print(f"[✓] Saved DB status snapshot to: {snapshot_local}")

    # Optional SCP backup of SQLite database if GCloud VM env vars are configured
    gcloud_host = os.environ.get("M5_GCLOUD_HOST", "")
    gcloud_zone = os.environ.get("M5_GCLOUD_ZONE", "")
    gcloud_proj = os.environ.get("M5_GCLOUD_PROJECT", "")
    remote_db = os.environ.get("M5_REMOTE_DB_PATH", "")
    if gcloud_host and remote_db:
        db_local = os.path.join(LOGS_DIR, "memory_backup.db")
        cmd = ["gcloud", "compute", "scp", f"{gcloud_host}:{remote_db}", db_local, "--quiet"]
        if gcloud_zone:
            cmd.append(f"--zone={gcloud_zone}")
        if gcloud_proj:
            cmd.append(f"--project={gcloud_proj}")
        res = subprocess.run(cmd)
        if res.returncode == 0:
            print(f"[✓] Synced full SQLite database backup to: {db_local}")

    print("\n--- Recent VM Log Tail ---")
    for line in logs_data.get("lines", [])[-15:]:
        print(" ", line)

    print("\n--- Recent Audio Chunks in DB ---")
    for c in status_data.get("recent_chunks", [])[:10]:
        print(f"  Chunk #{c['id']} [{c['date_nzst']} {c['time_nzst']}] {c['duration_sec']}s RMS={c['rms_raw']} [{c['status']}]: {c['transcript']}")


def monitor_serial():
    import serial
    ports = sorted(glob.glob("/dev/cu.usbserial-*"))
    if not ports:
        print("[!] No M5StickC Plus USB serial port found.")
        return
    port = ports[0]
    serial_log_path = os.path.join(LOGS_DIR, "device_serial.log")
    print(f"[*] Monitoring M5StickC Plus on {port} -> {serial_log_path} (Ctrl+C to stop)")
    s = serial.Serial(port, 115200, timeout=0.5)
    with open(serial_log_path, "a", encoding="utf-8") as lf:
        while True:
            line = s.readline().decode("utf-8", errors="replace").strip()
            if line:
                ts = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                entry = f"[{ts}] {line}"
                print(entry)
                lf.write(entry + "\n")
                lf.flush()


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "vm"
    if mode == "serial":
        monitor_serial()
    else:
        sync_vm()
