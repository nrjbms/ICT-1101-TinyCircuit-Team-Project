


#!/usr/bin/env python3
"""
Study Box server: Telegram bot + tiny HTTP API that the Tiny Circuit talks to.

Telegram commands
  /study 25    -> set 25 minutes (5-120, multiples of 5)
  /study 30s   -> 30 second test session
  /week        -> show this week's study log

Tiny Circuit endpoints (plain HTTP, same WiFi as the TinyShield)
  GET /poll                                         -> "SET <secs>" or "NONE"
  GET /event?type=start&secs=N&sid=ID               -> "OK"
  GET /event?type=complete&secs=N&distractions=D&sid=ID -> "OK"

Setup:  pip install requests
        BOT_TOKEN=123:abc python3 bot.py
"""
import json
import os
import threading
import time
from datetime import date, timedelta
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

import requests

BOT_TOKEN = "9121548197:AAGwltMP0RxCEWA6GgOhiTsOa_TyJWgRliw"
HTTP_PORT = 8080
DATA_FILE = "study_data.json"
MIN_DAYS_FOR_TABLE = 2   # show the weekly table once the box was used on this many days this week

API = f"https://api.telegram.org/bot{BOT_TOKEN}"
DAY_NAMES = ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"]

lock = threading.Lock()
state = {"chat_id": None, "pending_secs": None, "last_sid": None, "log": {}}


# ---------- storage ----------
def load():
    if os.path.exists(DATA_FILE):
        with open(DATA_FILE) as f:
            state.update(json.load(f))


def save():
    with open(DATA_FILE, "w") as f:
        json.dump(state, f, indent=2)


# ---------- formatting ----------
def fmt(secs):
    h, rem = divmod(int(secs), 3600)
    m, s = divmod(rem, 60)
    parts = []
    if h:
        parts.append(f"{h} hr")
    if m:
        parts.append(f"{m} min{'s' if m != 1 else ''}")
    if s:
        parts.append(f"{s} sec{'s' if s != 1 else ''}")
    return " ".join(parts) or "-"


def week_lines(today=None):
    today = today or date.today()
    sunday = today - timedelta(days=(today.weekday() + 1) % 7)   # week runs Sunday..Saturday
    lines, days_studied = [], 0
    for i, name in enumerate(DAY_NAMES):
        secs = state["log"].get((sunday + timedelta(days=i)).isoformat(), 0)
        if secs:
            days_studied += 1
        lines.append(f"{name}: {fmt(secs) if secs else '-'}")
    return lines, days_studied


def completion_message(distractions):
    msg = "Congratulations for completing your study session!\n"
    msg += f"You got distracted {distractions} time{'s' if distractions != 1 else ''}."
    lines, days = week_lines()
    if days >= MIN_DAYS_FOR_TABLE:
        msg += "\n\nStudy Session Logs:\n" + "\n".join(lines)
    return msg


def parse_duration(arg):
    arg = arg.strip().lower()
    if arg == "30s":
        return 30
    try:
        mins = int(arg)
    except ValueError:
        return None
    if 5 <= mins <= 120 and mins % 5 == 0:
        return mins * 60
    return None


# ---------- telegram ----------
def send(chat_id, text):
    if chat_id is None:
        return
    try:
        requests.post(f"{API}/sendMessage", json={"chat_id": chat_id, "text": text}, timeout=10)
    except requests.RequestException as e:
        print("sendMessage failed:", e)


HELP = ("Study Box bot\n"
        "/study 25 - set a 25 minute session (5-120 min, steps of 5)\n"
        "/study 30s - 30 second test session\n"
        "/week - this week's study log\n\n"
        "After setting the time, put your phone in the box and close the lid.")


def handle_message(msg):
    chat_id = msg["chat"]["id"]
    text = (msg.get("text") or "").strip()
    cmd, _, arg = text.partition(" ")
    cmd = cmd.split("@")[0].lower()

    with lock:
        state["chat_id"] = chat_id
        save()

    if cmd in ("/start", "/help"):
        send(chat_id, HELP)
    elif cmd == "/study":
        secs = parse_duration(arg)
        if secs is None:
            send(chat_id, "Please pick 5 to 120 minutes in steps of 5, e.g. /study 25 (or /study 30s to test).")
            return
        with lock:
            state["pending_secs"] = secs
        send(chat_id, f"Timer set to {fmt(secs)}. Put your phone in the box and close the lid to start.")
    elif cmd == "/week":
        lines, _ = week_lines()
        send(chat_id, "Study Session Logs:\n" + "\n".join(lines))


def telegram_loop():
    offset = None
    while True:
        try:
            r = requests.get(f"{API}/getUpdates", params={"timeout": 30, "offset": offset}, timeout=40).json()
            for u in r.get("result", []):
                offset = u["update_id"] + 1
                if "message" in u:
                    handle_message(u["message"])
        except (requests.RequestException, ValueError) as e:
            print("getUpdates failed:", e)
            time.sleep(3)


# ---------- device API ----------
def handle_event(q):
    try:
        secs = int(q.get("secs", 0))
        distractions = int(q.get("distractions", 0))
    except ValueError:
        return "BAD"
    sid = q.get("sid")
    with lock:
        chat_id = state["chat_id"]

    if q.get("type") == "start":
        send(chat_id, f"Study session started: {fmt(secs)}. Stay focused!")
    elif q.get("type") == "complete":
        with lock:
            if sid is not None and sid == state["last_sid"]:
                return "OK"                      # device retried; already counted
            state["last_sid"] = sid
            today = date.today().isoformat()
            state["log"][today] = state["log"].get(today, 0) + secs
            save()
        send(chat_id, completion_message(distractions))
    else:
        return "BAD"
    return "OK"


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        url = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(url.query).items()}
        code, body = 404, "NOT FOUND"
        if url.path == "/poll":
            with lock:
                secs, state["pending_secs"] = state["pending_secs"], None
            code, body = 200, (f"SET {secs}" if secs else "NONE")
        elif url.path == "/event":
            code, body = 200, handle_event(q)
        data = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    load()
    server = ThreadingHTTPServer(("0.0.0.0", HTTP_PORT), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    print(f"Device API on port {HTTP_PORT}. Put this PC's LAN IP in SERVER_HOST in the sketch.")
    telegram_loop()