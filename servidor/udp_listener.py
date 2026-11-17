# UDP Listener para DataLogger
import socket
import json
from datetime import datetime

UDP_IP = "0.0.0.0"
UDP_PORT = 5000

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind((UDP_IP, UDP_PORT))

print(f"=== Servidor UDP Listener escutando em {UDP_IP}:{UDP_PORT} ===")

while True:
    data, addr = sock.recvfrom(1024)
    payload = data.decode("utf-8")
    now = datetime.now().strftime("%H:%M:%S")
    
    try:
        json_data = json.loads(payload)
        print(f"[{now}] Recebido de {addr[0]}:{addr[1]} -> TS: {json_data.get("ts")} ms | IO: {json_data.get("io_val")} | State: {json_data.get("state")}")
    except json.JSONDecodeError:
        print(f"[{now}] Recebido de {addr[0]}:{addr[1]} -> (Raw): {payload}")
