"""EA FESL / Theater / Messenger handlers (hot-reloaded per connection)."""
import threading
import time

from ea_common import log, build, recv_packets, CFG, STATE


def local_ip(conn):
    """The server address the client reached us on (works for LAN/VPN/NAT)."""
    try:
        ip = conn.getsockname()[0]
        return ip if ip != "0.0.0.0" else CFG["host_ip"]
    except OSError:
        return CFG["host_ip"]


KEEPALIVE_SECS = 30


def keepalive(conn_obj, send_ping):
    """The client drops a connection that stays silent for 240 s (FESL
    "conn err -206"), so the server pings, as the real one did."""
    def loop():
        while not conn_obj.closed:
            time.sleep(KEEPALIVE_SECS)
            if conn_obj.closed:
                break
            try:
                send_ping()
            except OSError:
                break
    threading.Thread(target=loop, daemon=True).start()


class Fesl:
    def __init__(self, conn, addr):
        self.conn, self.addr = conn, addr
        self.user = "Player"
        self.last_flags = 0
        self.closed = False
        self.send_lock = threading.Lock()

    def send(self, ptype, tid_flags, fields, quiet=False):
        pkt = build(ptype, tid_flags, fields)
        if not quiet:
            log(f"FESL -> {ptype.decode()} {fields}")
        with self.send_lock:
            self.conn.sendall(pkt)

    def run(self):
        log(f"FESL connection from {self.addr}")
        keepalive(self, lambda: self.send(b"fsys", 0x80000000, {"TXN": "Ping"}, quiet=True))
        try:
            for ptype, flags, f in recv_packets(self.conn, b""):
                log(f"FESL <- {ptype.decode('latin1')} {flags:08X} {f}")
                reply = 0x80000000 | (flags & 0x00FFFFFF)
                self.last_flags = flags
                txn = f.get("TXN", "")
                handler = getattr(self, f"on_{ptype.decode('latin1')}_{txn}", None)
                if handler:
                    handler(ptype, reply, f)
                else:
                    log(f"  (unhandled {ptype} {txn}, echoing)")
                    self.send(ptype, reply, {"TXN": txn})
        except Exception as e:
            log(f"FESL error {e!r}")
        self.closed = True
        log("FESL closed")

    def on_fsys_Hello(self, ptype, reply, f):
        ip = local_ip(self.conn)
        self.send(b"fsys", reply, {
            "TXN": "Hello",
            "domainPartition.domain": "eagames",
            "domainPartition.subDomain": "AO2",
            "messengerIp": ip,
            "messengerPort": CFG["messenger_port"],
            "activityTimeoutSecs": 0,
            "curTime": time.strftime('"%b-%d-%Y %H:%M:%S UTC"', time.gmtime()),
            "theaterIp": ip,
            "theaterPort": CFG["theater_port"],
        })
        self.send(b"fsys", 0x80000000, {"TXN": "MemCheck", "memcheck.[]": 0, "type": 0, "salt": 5})

    def on_fsys_MemCheck(self, ptype, reply, f):
        pass  # client answer, nothing to send

    def on_fsys_Ping(self, ptype, reply, f):
        if self.last_flags & 0x40000000:  # the client's own ping (not an answer to ours)
            self.send(b"fsys", reply, {"TXN": "Ping"})

    def on_fsys_Goodbye(self, ptype, reply, f):
        pass

    def on_fsys_GetPingSites(self, ptype, reply, f):
        self.send(b"fsys", reply, {
            "TXN": "GetPingSites",
            "minPingSitesToPing": 0,
            "pingSites.[]": 1,
            "pingSites.0.addr": local_ip(self.conn),
            "pingSites.0.name": "local",
            "pingSites.0.type": 0,
        })

    def on_acct_XBL360Login(self, ptype, reply, f):
        self.user = f.get("gamertag", "Player")
        uid = int(f.get("xuid", "1")) & 0x7FFFFFFF
        with STATE["lock"]:
            STATE.setdefault("names", {})[uid] = self.user
            STATE.setdefault("xuids", {})[uid] = f.get("xuid", "0")
        self.send(b"acct", reply, {
            "TXN": "XBL360Login",
            "lkey": f"AO2LKEY{uid:010d}",
            "profileId": uid,
            "userId": uid,
            "nuid": self.user,
            "displayName": self.user,
            "personaName": self.user,
            "entitledGameFeatureWrappers.[]": 0,
        })


class Theater:
    def __init__(self, conn, addr):
        self.conn, self.addr = conn, addr
        self.name = "Player"
        self.uid = 0
        self.gid = 0
        self.xnaddr = ""
        self.closed = False
        self.send_lock = threading.Lock()

    def send(self, ptype, fields, status=0, quiet=False):
        """status: 0, or a 4-char error code in the header ('jden', 'ngam')."""
        if isinstance(status, str):
            status = int.from_bytes(status.encode("latin1"), "big")
        if not quiet:
            log(f"THEATER[{self.name}] -> {ptype.decode()}{f' [{status:08X}]' if status else ''} {fields}")
        with self.send_lock:
            self.conn.sendall(build(ptype, status, fields))

    def run(self):
        log(f"THEATER connection from {self.addr}")
        if type(self) is Theater:
            keepalive(self, lambda: self.send(b"PING", {"TID": 0}, quiet=True))
        try:
            for ptype, flags, f in recv_packets(self.conn, b""):
                name = ptype.decode("latin1")
                log(f"THEATER[{self.name}] <- {name} {flags:08X} {f}")
                handler = getattr(self, f"on_{name}", None)
                if handler:
                    handler(f)
                else:
                    log(f"  (unhandled theater {name}, echoing TID)")
                    self.send(ptype, {"TID": f.get("TID", 0)})
        except Exception as e:
            log(f"THEATER[{self.name}] error {e!r}")
        self.closed = True
        self.cleanup()
        log(f"THEATER[{self.name}] closed")

    def cleanup(self):
        with STATE["lock"]:
            if STATE["theaters"].get(self.uid) is self:
                del STATE["theaters"][self.uid]
            for gid in [g for g, game in STATE["games"].items() if game["host"] is self]:
                log(f"removing game {gid}")
                del STATE["games"][gid]

    # --- session -----------------------------------------------------------
    def on_CONN(self, f):
        self.send(b"CONN", {"TID": f.get("TID", 1), "TIME": int(time.time()),
                            "activityTimeoutSecs": 0, "PROT": f.get("PROT", 2)})

    def on_USER(self, f):
        # LKEY is "AO2LKEY<userId>" (see Fesl.on_acct_XBL360Login); the host
        # recognizes its own join request by this userId (EGRQ UID).
        lkey = f.get("LKEY", "")
        digits = lkey.rsplit("LKEY", 1)[-1]
        self.uid = int(digits) if digits.isdigit() else id(self) & 0x7FFFFFFF
        with STATE["lock"]:
            known = STATE.setdefault("names", {}).get(self.uid)
        self.name = f.get("NAME") or known or f"user{self.uid}"
        with STATE["lock"]:
            STATE["theaters"][self.uid] = self
        self.send(b"USER", {"TID": f.get("TID", 2), "NAME": self.name, "CID": ""})

    def on_PING(self, f):
        pass

    def on_LLST(self, f):
        tid = f.get("TID", 0)
        with STATE["lock"]:
            ngames = len(STATE["games"])
        self.send(b"LLST", {"TID": tid, "NUM-LOBBIES": 1})
        self.send(b"LDAT", {"TID": tid, "FAVORITE-GAMES": 0, "FAVORITE-PLAYERS": 0, "LID": 1,
                            "LOCALE": "en_US", "MAX-GAMES": 10000, "NAME": "ao2eu360",
                            "NUM-GAMES": ngames, "PASSING": ngames})

    def on_PCNT(self, f):
        with STATE["lock"]:
            players = sum(g["joined"] for g in STATE["games"].values())
        self.send(b"PCNT", {"TID": f.get("TID", 0), "LID": f.get("LID", 1), "COUNT": players})

    # --- hosting -----------------------------------------------------------
    def on_CGAM(self, f):
        with STATE["lock"]:
            gid = STATE["next_gid"]
            STATE["next_gid"] += 1
            game = {
                "gid": gid, "host": self, "fields": dict(f), "joined": 1,
                "ip": self.addr[0], "port": f.get("PORT", "1000"),
                "int_ip": f.get("INT-IP", self.addr[0]), "int_port": f.get("INT-PORT", "1000"),
                "max": f.get("MAX-PLAYERS", "2"), "name": f.get("NAME", self.name),
                "ugid": f.get("UGID", ""), "secret": f.get("SECRET", ""),
                "xnaddr": "",
            }
            STATE["games"][gid] = game
        self.gid = gid
        log(f"game {gid} created by {self.name}")
        self.send(b"CGAM", {"TID": f.get("TID", 0), "LID": 1, "GID": gid,
                            "MAX-PLAYERS": game["max"], "EKEY": "AO2EKEY", "UGID": game["ugid"],
                            "SECRET": game["secret"], "JOIN": "O", "J": "O", "ISXBL": 1})

    def on_UGAM(self, f):
        with STATE["lock"]:
            game = STATE["games"].get(self.gid)
            if game:
                game["fields"].update(f)
        # no reply expected

    def on_UBRA(self, f):
        self.send(b"UBRA", {"TID": f.get("TID", 0)})

    def on_RGAM(self, f):
        with STATE["lock"]:
            STATE["games"].pop(self.gid, None)
        self.send(b"RGAM", {"TID": f.get("TID", 0)})

    # --- browsing ------------------------------------------------------------
    def game_summary(self, game, tid):
        g = game["fields"]
        d = {"TID": tid, "LID": 1, "GID": game["gid"], "HN": game["host"].name,
             "HU": game["host"].uid, "N": game["name"], "I": game["ip"], "P": game["port"],
             "JP": game["joined"], "QP": 0, "AP": game["joined"], "MP": game["max"],
             "PL": "XBOX360", "TYPE": g.get("TYPE", "G"), "J": "O", "F": 0, "NF": 0, "PW": 0}
        for k, v in g.items():
            if k.startswith("B-") or k == "XB360SESS":
                d[k] = v
        return d

    def on_GLST(self, f):
        tid = f.get("TID", 0)
        with STATE["lock"]:
            games = [g for g in STATE["games"].values() if g["host"] is not self]
        self.send(b"GLST", {"TID": tid, "LID": f.get("LID", 1), "LOBBY-NUM-GAMES": len(games),
                            "LOBBY-MAX-GAMES": 10000, "FAVORITE-GAMES": 0,
                            "FAVORITE-PLAYERS": 0, "NUM-GAMES": len(games)})
        for g in games:
            self.send(b"GDAT", self.game_summary(g, tid))

    def on_GDAT(self, f):
        tid = f.get("TID", 0)
        gid = int(f.get("GID", 0) or 0)
        with STATE["lock"]:
            game = STATE["games"].get(gid)
        if not game:
            self.send(b"GDAT", {"TID": tid})
            return
        self.send(b"GDAT", self.game_summary(game, tid))
        self.send(b"GDET", {"TID": tid, "LID": 1, "GID": gid, "UGID": game["ugid"]})

    # --- joining -------------------------------------------------------------
    # Entering a game. Everybody, the host included, goes through the same
    # round: EGAM ack, EGRQ to the host (the host also gets its own request and
    # admits itself), the host's EGRS, then EGEG with the host's address to
    # the player. The host's game manager only starts hosting (and admitting
    # others) once it has received its own EGEG; before that it refuses
    # requests with REASON=2.
    def on_EGAM(self, f):
        tid = f.get("TID", 0)
        gid = int(f.get("GID", 0) or 0) or self.gid
        self.xnaddr = f.get("R-XNADDR", "")
        with STATE["lock"]:
            game = STATE["games"].get(gid)
            if game and game["host"] is self:
                game["xnaddr"] = self.xnaddr
            pid = STATE["next_pid"]
            STATE["next_pid"] += 1
            if game:
                STATE["pending"][pid] = self
        if not game:
            self.send(b"EGAM", {"TID": tid, "LID": 1, "GID": gid}, status="ngam")
            return
        self.send(b"EGAM", {"TID": tid, "LID": 1, "GID": gid})
        # XUID: the host registers the player in its Xbox Live session with it
        # (XSessionJoinRemote); without it the partner joins as XUID 0.
        with STATE["lock"]:
            xuid = STATE.setdefault("xuids", {}).get(self.uid, f.get("R-U-XUID", "0"))
        req = {"TID": 0, "NAME": self.name, "UID": self.uid, "XUID": xuid, "PID": pid,
               "TICKET": f"{pid:010d}", "IP": self.addr[0], "PORT": f.get("PORT", "1000"),
               "INT-IP": f.get("R-INT-IP", self.addr[0]),
               "INT-PORT": f.get("R-INT-PORT", f.get("PORT", "1000")),
               "PTYPE": f.get("PTYPE", "P"), "R-XNADDR": self.xnaddr, "LID": 1, "GID": gid}
        for k, v in f.items():
            if k.startswith("R-U-") or k.startswith("R-USER"):
                req[k] = v
        game["host"].send(b"EGRQ", req)

    def on_EGRS(self, f):
        pid = int(f.get("PID", 0) or 0)
        with STATE["lock"]:
            joiner = STATE["pending"].pop(pid, None)
            game = STATE["games"].get(self.gid)
        self.send(b"EGRS", {"TID": f.get("TID", 0)})
        if not joiner or not game:
            return
        if f.get("ALLOWED", "1") != "1":
            joiner.send(b"EGEG", {"TID": 0, "LID": 1, "GID": game["gid"],
                                  "REASON": f.get("REASON", "0")}, status="jden")
            return
        if joiner is not self:
            with STATE["lock"]:
                game["joined"] += 1
        joiner.send(b"EGEG", {"TID": 0, "PL": "xbox360", "TICKET": f"{pid:010d}", "PID": pid,
                              "HUID": self.uid, "I": game["ip"], "P": game["port"],
                              "INT-IP": game["int_ip"], "INT-PORT": game["int_port"],
                              "EKEY": "AO2EKEY", "UGID": game["ugid"], "SECRET": game["secret"],
                              "XNADDR": game["xnaddr"], "XB360SESS": game["fields"].get("XB360SESS", ""),
                              "LID": 1, "GID": game["gid"]})

    def on_ECNL(self, f):
        self.send(b"ECNL", {"TID": f.get("TID", 0), "LID": f.get("LID", 1),
                            "GID": f.get("GID", 0)})

    def on_PENT(self, f):
        self.send(b"PENT", {"TID": f.get("TID", 0), "PID": f.get("PID", 0)})

    def on_PLVT(self, f):
        with STATE["lock"]:
            game = STATE["games"].get(self.gid)
            if game and game["joined"] > 1:
                game["joined"] -= 1
        self.send(b"PLVT", {"TID": f.get("TID", 0)})


class Messenger(Theater):
    def run(self):
        log(f"MESSENGER connection from {self.addr}")
        try:
            for ptype, flags, f in recv_packets(self.conn, b""):
                name = ptype.decode("latin1")
                log(f"MESSENGER <- {name} {flags:08X} {f}")
                if name == "AUTH":
                    self.send(ptype, {"TITL": "Army of Two", "ID": f.get("ID", 1)})
                elif name == "PING":
                    pass
                else:
                    self.send(ptype, {"ID": f.get("ID", 0)})
        except Exception as e:
            log(f"MESSENGER error {e!r}")
        log("MESSENGER closed")
