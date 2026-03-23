import socket
import time
from datetime import datetime

IP = "192.168.178.55"
PORT = 2323
LOG_FILE = "bms_bridge.log"

def start_logging():
    print(f"Connecting to BMS Bridge at {IP}:{PORT}...")
    while True:
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.settimeout(15)
                s.connect((IP, PORT))
                print(f"Connected! Logging to {LOG_FILE}...")
                
                buffer = ""
                while True:
                    data = s.recv(1024).decode('utf-8', errors='ignore')
                    if not data: break
                    
                    buffer += data
                    if "\n" in buffer:
                        lines = buffer.split("\n")
                        buffer = lines.pop()
                        with open(LOG_FILE, "a") as f:
                            for line in lines:
                                if line.strip():
                                    ts = datetime.now().strftime("[%Y-%m-%d %H:%M:%S]")
                                    f.write(f"{ts} {line.strip()}\n")
                            f.flush()
        except Exception as e:
            print(f"Connection lost ({e}). Retrying in 5s...")
            time.sleep(5)

if __name__ == "__main__":
    start_logging()