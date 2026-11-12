import html
import json
import os
import random
import threading
import time
from datetime import datetime, timedelta
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import requests

# -------------------- SETTINGS --------------------
BOT_TOKEN = os.getenv("TELEGRAM_BOT_TOKEN", "TELEGRAM_BOT_TOKEN")
CHAT_ID = os.getenv("TELEGRAM_CHAT_ID", "TELEGRAM_CHAT_ID")
HOST = "0.0.0.0"
PORT = 8080
DATA_FILE = Path(__file__).with_name("study_data.json")
QUOTES_FILE = Path(__file__).with_name("quotes.json")
TELEGRAM_API = f"https://api.telegram.org/bot{BOT_TOKEN}"

# Arduino sends /poll and /event requests to this server.
lock = threading.Lock()
pending_study_seconds = None
sessions = {}


def load_data():
    """Load session history, accepting either the new object or a legacy list."""
    if not DATA_FILE.exists():
        return []
    try:
        with DATA_FILE.open("r", encoding="utf-8") as file:
            data = json.load(file)
        if isinstance(data, list):
            return data
        if isinstance(data, dict):
            records = data.get("sessions", data.get("records", []))
            return records if isinstance(records, list) else []
    except (OSError, json.JSONDecodeError) as error:
        print(f"Could not read {DATA_FILE.name}: {error}")
    return []


def save_data(records):
    temp_file = DATA_FILE.with_suffix(".tmp")
    with temp_file.open("w", encoding="utf-8") as file:
        json.dump({"sessions": records}, file, indent=2, ensure_ascii=False)
    temp_file.replace(DATA_FILE)


def load_quotes():
    try:
        with QUOTES_FILE.open("r", encoding="utf-8") as file:
            data = json.load(file)
        quotes = data.get("quotes", []) if isinstance(data, dict) else data
        return [str(q).strip() for q in quotes if str(q).strip()]
    except (OSError, json.JSONDecodeError, AttributeError):
        return ["Focus on Progress, Not Perfection"]


def format_duration(seconds):
    seconds = max(0, int(round(float(seconds))))
    hours, remainder = divmod(seconds, 3600)
    minutes, secs = divmod(remainder, 60)
    parts = []
    if hours:
        parts.append(f"{hours} hr" if hours == 1 else f"{hours} hrs")
    if minutes:
        parts.append(f"{minutes} min" if minutes == 1 else f"{minutes} mins")
    if secs and not hours:
        parts.append(f"{secs} sec" if secs == 1 else f"{secs} secs")
    return " ".join(parts) if parts else "0 mins"


def parse_timestamp(value):
    if not value:
        return None
    try:
        return datetime.fromisoformat(value)
    except (TypeError, ValueError):
        return None


def telegram_send(message):
    if not BOT_TOKEN or BOT_TOKEN.startswith("PASTE_") or not CHAT_ID or CHAT_ID.startswith("PASTE_"):
        print("Telegram message not sent: set TELEGRAM_BOT_TOKEN and TELEGRAM_CHAT_ID.")
        print(message)
        return False
    try:
        response = requests.post(
            f"{TELEGRAM_API}/sendMessage",
            data={"chat_id": CHAT_ID, "text": message, "parse_mode": "HTML"},
            timeout=15,
        )
        response.raise_for_status()
        return response.json().get("ok", False)
    except requests.RequestException as error:
        print(f"Telegram send error: {error}")
        return False


def build_week_report():
    now = datetime.now().astimezone()
    monday = (now - timedelta(days=now.weekday())).date()
    weekdays = [monday + timedelta(days=i) for i in range(7)]
    records = load_data()
    lines = ["<b>Study Session Logs:</b>"]

    for day in weekdays:
        lines.append("---------------------------------")
        lines.append(f"<b>{day.strftime('%A')}:</b>")
        day_records = []
        for record in records:
            start = parse_timestamp(record.get("start_at"))
            if start and start.astimezone().date() == day:
                day_records.append((start, record))
        day_records.sort(key=lambda item: item[0])

        if not day_records:
            lines.append("-")
            continue

        for index, (start, record) in enumerate(day_records, start=1):
            end = parse_timestamp(record.get("end_at"))
            if end is None:
                end_text = "In progress"
            else:
                end_text = end.astimezone().strftime("%H%M")
            start_text = start.astimezone().strftime("%H%M")
            duration = format_duration(record.get("study_seconds", 0))
            lines.append(f"Session {index}: {start_text} - {end_text} [{duration}]")

    lines.append("---------------------------------")
    return "\n".join(lines)


def valid_chat(chat_id):
    return not CHAT_ID or CHAT_ID.startswith("PASTE_") or str(chat_id) == str(CHAT_ID)


def handle_command(message):
    global pending_study_seconds
    chat_id = message.get("chat", {}).get("id")
    if chat_id is None or not valid_chat(chat_id):
        return

    text = (message.get("text") or "").strip()
    if not text:
        return
    command = text.split()[0].split("@")[0].lower()
    args = text.split()[1:]

    if command == "/start":
        telegram_send(
            "<b>Study Box is ready.</b>\n"
            "Use <code>/study 25</code> to set 25 minutes,\n"
            "or <code>/study 30s</code> for a 30-second test.\n"
            "Use <code>/week</code> to view this week's logs."
        )
    elif command == "/study":
        if len(args) != 1:
            telegram_send("Usage: <code>/study 25</code> or <code>/study 30s</code>")
            return
        value = args[0].lower()
        if value in ("30s", "30sec", "30secs"):
            seconds = 30
        else:
            try:
                minutes = int(value)
                seconds = minutes * 60
            except ValueError:
                telegram_send("Please enter a number of minutes, or <code>30s</code>.")
                return
            if minutes < 5 or minutes > 120 or minutes % 5 != 0:
                telegram_send("Choose 5 to 120 minutes in 5-minute increments, or use <code>30s</code>.")
                return
        with lock:
            pending_study_seconds = seconds
        telegram_send(f"Study timer set to <b>{html.escape(format_duration(seconds))}</b>. It will be sent to your Study Box.")
    elif command == "/week":
        telegram_send(build_week_report())
    else:
        telegram_send("Commands: <code>/study 25</code>, <code>/study 30s</code>, <code>/week</code>")


def telegram_polling():
    if BOT_TOKEN.startswith("PASTE_"):
        print("Set TELEGRAM_BOT_TOKEN before using Telegram commands.")
        return
    offset = None
    while True:
        try:
            params = {"timeout": 20}
            if offset is not None:
                params["offset"] = offset
            response = requests.get(f"{TELEGRAM_API}/getUpdates", params=params, timeout=25)
            response.raise_for_status()
            payload = response.json()
            if payload.get("ok"):
                for update in payload.get("result", []):
                    offset = update["update_id"] + 1
                    if "message" in update:
                        handle_command(update["message"])
        except requests.RequestException as error:
            print(f"Telegram polling error: {error}")
            time.sleep(3)


class StudyBoxHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        global pending_study_seconds
        parsed = urlparse(self.path)
        params = parse_qs(parsed.query)

        if parsed.path == "/poll":
            with lock:
                seconds = pending_study_seconds
                pending_study_seconds = None
            self.reply("SET " + str(seconds) if seconds is not None else "OK")
            return

        if parsed.path == "/event":
            event_type = params.get("type", [""])[0]
            sid = params.get("sid", [""])[0]
            now = datetime.now().astimezone()

            if event_type == "start":
                seconds = int(params.get("secs", ["0"])[0])
                with lock:
                    sessions[sid] = {
                        "sid": sid,
                        "start_at": now.isoformat(timespec="seconds"),
                        "end_at": None,
                        "study_seconds": seconds,
                        "distractions": 0,
                        "distraction_seconds": 0,
                    }
                self.reply("STARTED")
                return

            if event_type == "complete":
                distractions = int(params.get("distractions", ["0"])[0])
                lost_ms = int(params.get("lost_ms", ["0"])[0])
                with lock:
                    record = sessions.pop(sid, None)
                if record is None:
                    record = {
                        "sid": sid,
                        "start_at": now.isoformat(timespec="seconds"),
                        "study_seconds": int(params.get("secs", ["0"])[0]),
                    }
                record["end_at"] = now.isoformat(timespec="seconds")
                record["distractions"] = distractions
                record["distraction_seconds"] = max(0, lost_ms // 1000)
                records = load_data()
                # Arduino retries the completion request if the response is lost.
                # Do not create duplicate weekly entries or send duplicate messages.
                already_saved = any(str(item.get("sid", "")) == str(sid) for item in records)
                if not already_saved:
                    records.append(record)
                    save_data(records)

                if already_saved:
                    self.reply("OK")
                    return

                quotes = load_quotes()
                quote = random.choice(quotes) if quotes else "Focus on Progress, Not Perfection"
                lost_text = format_duration(record["distraction_seconds"])
                message = (
                    "<b>Congratulations for completing your study session!</b>\n\n"
                    f"You got distracted <b>{distractions}</b> times.\n\n"
                    f"You got distracted for a total of <b>{html.escape(lost_text)}</b> during your session.\n\n"
                    f"<i>“{html.escape(quote)}”</i>"
                )
                telegram_send(message)
                self.reply("OK")
                return

            self.reply("UNKNOWN EVENT")
            return

        if parsed.path == "/health":
            self.reply("OK")
            return

        self.reply("NOT FOUND", status=404)

    def reply(self, body, status=200):
        encoded = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(encoded)

    def log_message(self, fmt, *args):
        print(f"HTTP: {fmt % args}")


def main():
    print(f"Study Box server listening on http://0.0.0.0:{PORT}")
    print(f"Session data: {DATA_FILE}")
    print(f"Quotes: {QUOTES_FILE}")
    threading.Thread(target=telegram_polling, daemon=True).start()
    server = ThreadingHTTPServer((HOST, PORT), StudyBoxHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStudy Box server stopped.")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
