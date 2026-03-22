import socket
import time
from datetime import datetime

# --- CONFIGURATION ---
IP = "192.168.178.55"
PORT = 23
LOG_FILE = "bms_bridge.log"

def start_logging():
    print(f"Connecting to {IP}...")
    while True:
        try:
            # Create socket and connect
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.settimeout(10)
                s.connect((IP, PORT))
                print(f"Connected! Logging to {LOG_FILE}...")
                
                # Receive data
                buffer = ""
                while True:
                    data = s.recv(1024).decode('utf-8', errors='ignore')
                    if not data:
                        break # Connection closed by peer
                    
                    buffer += data
                    if "\n" in buffer:
                        lines = buffer.split("\n")
                        # Keep the last partial line in the buffer
                        buffer = lines.pop()
                        
                        with open(LOG_FILE, "a") as f:
                            for line in lines:
                                timestamp = datetime.now().strftime("[%Y-%m-%d %H:%M:%S]")
                                formatted_line = f"{timestamp} {line.strip()}"
                                print(formatted_line) # Print to terminal
                                f.write(formatted_line + "\n") # Save to file
                                f.flush() # Ensure it writes to disk immediately
        
        except (socket.error, socket.timeout):
            print("Connection lost. Retrying in 5 seconds...")
            time.sleep(5)
        except KeyboardInterrupt:
            print("\nLogging stopped by user.")
            break

if __name__ == "__main__":
    start_logging()