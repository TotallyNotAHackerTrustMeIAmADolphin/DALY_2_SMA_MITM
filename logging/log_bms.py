import socket
import time
from datetime import datetime

IP = "192.168.178.55"
PORT = 2323
LOG_FILE = "bms_bridge.log"

def write_to_log(message):
    ts = datetime.now().strftime("[%Y-%m-%d %H:%M:%S]")
    try:
        with open(LOG_FILE, "a") as f:
            f.write(f"{ts} {message}\n")
            # f.flush() is implicit here when the 'with' block exits
    except Exception as e:
        print(f"Failed to write to local file: {e}")

def start_logging():
    print(f"Connecting to BMS Bridge at {IP}:{PORT}...")
    while True:
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.settimeout(15)
                s.connect((IP, PORT))
                
                msg = f"--- CONNECTION ESTABLISHED TO {IP} ---"
                print(msg)
                write_to_log(msg)
                
                buffer = ""
                while True:
                    data = s.recv(1024).decode('utf-8', errors='ignore')
                    if not data: 
                        break 
                    
                    buffer += data
                    if "\n" in buffer:
                        lines = buffer.split("\n")
                        # Keep the last partial line in the buffer
                        buffer = lines.pop()
                        for line in lines:
                            clean_line = line.strip()
                            if clean_line:
                                write_to_log(clean_line)
                                # Also print to screen so you see it live
                                print(f"{datetime.now().strftime('%H:%M:%S')} {clean_line}")
                                
        except (socket.timeout, ConnectionRefusedError, socket.error):
            err_msg = "--- CONNECTION LOST. RETRYING IN 5S... ---"
            print(err_msg)
            write_to_log(err_msg)
            time.sleep(5)
        except Exception as e:
            err_msg = f"--- LOGGER ERROR: {e} ---"
            print(err_msg)
            write_to_log(err_msg)
            time.sleep(5)

if __name__ == "__main__":
    start_logging()