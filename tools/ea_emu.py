"""EA server emulator entry point. Usage: python ea_emu.py [advertised_ip]"""
import importlib
import socket
import threading
import time

import ea_common
import ea_handlers
from ea_common import log, CFG


def serve(port, cls_name):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", port))
    s.listen(16)
    log(f"listening {cls_name} on {port}")
    while True:
        c, a = s.accept()
        try:
            mod = importlib.reload(ea_handlers)
        except Exception as e:
            log(f"reload failed: {e}")
            mod = ea_handlers
        cls = getattr(mod, cls_name)
        threading.Thread(target=lambda: cls(c, a).run(), daemon=True).start()


threading.Thread(target=serve, args=(CFG["fesl_port"], "Fesl"), daemon=True).start()
threading.Thread(target=serve, args=(CFG["theater_port"], "Theater"), daemon=True).start()
threading.Thread(target=serve, args=(CFG["messenger_port"], "Messenger"), daemon=True).start()
while True:
    time.sleep(1)
