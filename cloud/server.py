#!/usr/bin/env python3
"""
M5StickC Plus — Continuous Audio Logger & Daily Context Engine
Runs on any Linux VM / Cloud server:
- Receives live or buffered ADPCM/PCM audio chunks from M5StickC Plus over HTTP
- Normalizes audio (DC offset removal + high-pass filter + auto-gain) and transcribes via Gemini
- Stores transcripts, sessions, daily summaries, and evolving long-term memory in SQLite
- Sends rich daily HTML briefing emails with attached full verbatim transcript
"""

import base64
import datetime
import hmac
import html
import io
import json
import math
import os
import queue
import re
import smtplib
import sqlite3
import struct
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import wave
from email.mime.multipart import MIMEMultipart
from email.mime.text import MIMEText
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from zoneinfo import ZoneInfo

# ─── Configuration (Loaded from Environment or Local .env File) ───────────────
BASE_DIR = os.path.dirname(os.path.abspath(__file__))


def _load_dotenv(env_path: str):
    if not os.path.isfile(env_path):
        return
    with open(env_path, "r", encoding="utf-8") as f:
        for raw_line in f:
            line = raw_line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            k = k.strip()
            v = v.strip()
            if (len(v) >= 2) and ((v[0] == v[-1] == '"') or (v[0] == v[-1] == "'")):
                v = v[1:-1]
            os.environ.setdefault(k, v)


_load_dotenv(os.path.join(BASE_DIR, ".env"))

HOST = os.environ.get("HOST", "0.0.0.0")
PORT = int(os.environ.get("PORT", "8765"))
DB_PATH = os.path.join(BASE_DIR, "memory.db")
LOG_FILE_PATH = os.path.join(BASE_DIR, "audio_brain.log")
DEVICE_LOG_FILE_PATH = os.path.join(BASE_DIR, "m5stick_device.log")
AUDIO_TMP_DIR = os.path.join(BASE_DIR, "recent_audio")

DEVICE_TOKEN = os.environ.get("DEVICE_TOKEN", "")
DASHBOARD_USER = os.environ.get("DASHBOARD_USER", "admin")
DASHBOARD_PASSWORD = os.environ.get("DASHBOARD_PASSWORD", "")

RECIPIENT_EMAIL = os.environ.get("RECIPIENT_EMAIL", "")
SENDER_EMAIL = os.environ.get("SENDER_EMAIL", RECIPIENT_EMAIL)
SENDER_PASSWORD = os.environ.get("SENDER_PASSWORD", "")
SMTP_HOST = os.environ.get("SMTP_HOST", "smtp.gmail.com")
SMTP_PORT = int(os.environ.get("SMTP_PORT", "587"))
GEMINI_API_KEY = os.environ.get("GEMINI_API_KEY", "")
TIMEZONE_NAME = os.environ.get("TIMEZONE", "Pacific/Auckland")

GEMINI_MODELS = [
    "gemini-3.5-flash",
    "gemini-3.5-flash-lite",
    "gemini-3.1-flash-lite",
]

NZ_TZ = ZoneInfo(TIMEZONE_NAME)
os.makedirs(AUDIO_TMP_DIR, exist_ok=True)

# Background transcription queue & telemetry state
transcription_queue = queue.Queue()
db_lock = threading.Lock()
last_web_summary_epoch = 0.0
device_telemetry = {
    "recording": False,
    "vbat": "",
    "usb": False,
    "ip": "",
    "ssid": "",
    "last_seen_nzst": "",
    "last_seen_epoch": 0.0,
}


def is_valid_device_token(provided: str) -> bool:
    if not DEVICE_TOKEN or not provided:
        return False
    return hmac.compare_digest(DEVICE_TOKEN.encode("utf-8"), provided.encode("utf-8"))


def check_dashboard_auth(headers) -> bool:
    if not DASHBOARD_PASSWORD:
        return True
    auth_hdr = headers.get("Authorization", "")
    if not auth_hdr.startswith("Basic "):
        return False
    try:
        decoded = base64.b64decode(auth_hdr[6:].strip()).decode("utf-8")
        user, pwd = decoded.split(":", 1)
        return hmac.compare_digest(user, DASHBOARD_USER) and hmac.compare_digest(pwd, DASHBOARD_PASSWORD)
    except Exception:
        return False


def now_nz():
    return datetime.datetime.now(NZ_TZ)


def log_event(msg: str):
    ts = now_nz().strftime("%Y-%m-%d %H:%M:%S NZT")
    line = f"[{ts}] {msg}"
    print(line, flush=True)
    try:
        with open(LOG_FILE_PATH, "a", encoding="utf-8") as lf:
            lf.write(line + "\n")
    except Exception:
        pass


def get_db():
    conn = sqlite3.connect(DB_PATH, timeout=30)
    conn.row_factory = sqlite3.Row
    return conn


def init_db():
    with db_lock:
        conn = get_db()
        cur = conn.cursor()
        cur.executescript(
            """
            CREATE TABLE IF NOT EXISTS sessions (
                session_id TEXT PRIMARY KEY,
                started_at_nzst TEXT,
                last_chunk_at_nzst TEXT,
                chunks_count INTEGER DEFAULT 0,
                speech_chunks_count INTEGER DEFAULT 0,
                total_seconds REAL DEFAULT 0.0
            );

            CREATE TABLE IF NOT EXISTS chunks (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                session_id TEXT,
                date_nzst TEXT,
                time_nzst TEXT,
                created_at_utc TEXT,
                duration_sec REAL,
                rms_raw INTEGER,
                rms_norm INTEGER,
                status TEXT,
                transcript TEXT,
                wav_path TEXT
            );

            CREATE TABLE IF NOT EXISTS daily_summaries (
                date_nzst TEXT PRIMARY KEY,
                generated_at_nzst TEXT,
                total_chunks INTEGER,
                speech_chunks INTEGER,
                speech_minutes REAL,
                summary_markdown TEXT,
                summary_html TEXT,
                email_sent INTEGER DEFAULT 0,
                last_emailed_chunk_id INTEGER DEFAULT 0
            );

            CREATE TABLE IF NOT EXISTS long_term_context (
                id INTEGER PRIMARY KEY CHECK (id = 1),
                updated_at_nzst TEXT,
                days_summarized INTEGER DEFAULT 0,
                context_markdown TEXT
            );

            INSERT OR IGNORE INTO long_term_context (id, updated_at_nzst, days_summarized, context_markdown)
            VALUES (1, '', 0, 'No historical context built yet. This knowledge base will grow automatically each day.');
            """
        )
        conn.commit()
        # Ensure migration for last_emailed_chunk_id on existing DB
        try:
            cur.execute("ALTER TABLE daily_summaries ADD COLUMN last_emailed_chunk_id INTEGER DEFAULT 0")
            conn.commit()
        except Exception:
            pass
        # Clean up any legacy <noise> tokens stored as SPEECH
        try:
            cur.execute(
                "UPDATE chunks SET status='SILENCE', transcript='' WHERE TRIM(LOWER(transcript)) IN ('<noise>', '[noise]', '[silence]', '<silence>')"
            )
            conn.commit()
        except Exception:
            pass
        conn.close()


# ─── IMA ADPCM 4-bit Decoder (for Offline Store-and-Forward Clips) ──────────
IMA_INDEX_TABLE = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]
IMA_STEP_TABLE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
]


def decode_adpcm_to_pcm16(adpcm_bytes: bytes) -> bytes:
    """Decodes 4-bit IMA ADPCM bytes (low nibble first, then high nibble) into 16-bit signed PCM bytes."""
    predictor = 0
    step_index = 0
    out_samples = []

    for b in adpcm_bytes:
        for shift in (0, 4):
            nibble = (b >> shift) & 0x0F
            step = IMA_STEP_TABLE[step_index]
            diff = step >> 3
            if nibble & 1:
                diff += step >> 2
            if nibble & 2:
                diff += step >> 1
            if nibble & 4:
                diff += step
            if nibble & 8:
                predictor -= diff
            else:
                predictor += diff

            if predictor > 32767:
                predictor = 32767
            elif predictor < -32768:
                predictor = -32768

            step_index += IMA_INDEX_TABLE[nibble]
            if step_index < 0:
                step_index = 0
            elif step_index > 88:
                step_index = 88

            out_samples.append(predictor)

    return struct.pack(f"<{len(out_samples)}h", *out_samples)


# ─── Audio Processing (DC Offset Removal + Gain Normalization + WAV) ─────────
def pcm16_to_normalized_wav(raw_pcm: bytes, sample_rate: int = 16000):
    """
    Takes raw 16-bit signed little-endian mono PCM bytes from the M5StickC Plus SPM1423 PDM mic,
    removes DC offset, applies a gentle high-pass filter & automatic gain control (AGC),
    and returns (wav_bytes, duration_sec, rms_raw, rms_norm).
    """
    if len(raw_pcm) % 2 == 1:
        raw_pcm = raw_pcm[:-1]
    num_samples = len(raw_pcm) // 2
    if num_samples == 0:
        return b"", 0.0, 0, 0

    samples = list(struct.unpack(f"<{num_samples}h", raw_pcm))

    # 1. Remove DC offset (PDM mics on ESP32 often have a strong DC bias)
    mean_val = sum(samples) // num_samples
    centered = [s - mean_val for s in samples]

    # 2. First-order high-pass filter (~80Hz cutoff at 16kHz) to remove rumble/hum
    filtered = [0] * num_samples
    prev_x = centered[0]
    prev_y = 0.0
    alpha = 0.969
    sum_sq_raw = 0
    max_abs = 1
    for i, x in enumerate(centered):
        y = alpha * (prev_y + x - prev_x)
        prev_x = x
        prev_y = y
        yi = int(y)
        filtered[i] = yi
        sum_sq_raw += yi * yi
        if abs(yi) > max_abs:
            max_abs = abs(yi)

    rms_raw = int(math.sqrt(sum_sq_raw / num_samples))

    # 3. Automatic Gain Control (normalize peak to ~26000, up to 36x boost for desk-distance speech)
    target_peak = 26000.0
    gain = min(36.0, max(1.0, target_peak / float(max_abs)))
    if rms_raw < 20:
        # Very quiet digital silence — keep modest gain
        gain = min(gain, 4.0)

    norm_samples = [0] * num_samples
    sum_sq_norm = 0
    for i, y in enumerate(filtered):
        val = int(y * gain)
        if val > 32767:
            val = 32767
        elif val < -32768:
            val = -32768
        norm_samples[i] = val
        sum_sq_norm += val * val

    rms_norm = int(math.sqrt(sum_sq_norm / num_samples))
    duration_sec = round(num_samples / float(sample_rate), 2)

    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(struct.pack(f"<{num_samples}h", *norm_samples))

    return buf.getvalue(), duration_sec, rms_raw, rms_norm


# ─── Gemini API Helpers ──────────────────────────────────────────────────────
def call_gemini(parts, temperature=0.2, timeout=45):
    if not GEMINI_API_KEY:
        raise RuntimeError("GEMINI_API_KEY not configured")

    body = {
        "contents": [{"parts": parts}],
        "generationConfig": {"temperature": temperature},
    }
    body_bytes = json.dumps(body).encode("utf-8")

    last_err = None
    for model in GEMINI_MODELS:
        url = f"https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent"
        headers = {
            "Content-Type": "application/json",
            "X-goog-api-key": GEMINI_API_KEY,
        }
        try:
            req = urllib.request.Request(url, data=body_bytes, headers=headers, method="POST")
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                data = json.loads(resp.read().decode("utf-8"))
                candidates = data.get("candidates", [])
                if candidates:
                    text_parts = candidates[0].get("content", {}).get("parts", [])
                    text = "".join(p.get("text", "") for p in text_parts).strip()
                    if text:
                        return text, model
        except urllib.error.HTTPError as e:
            err_body = ""
            try:
                err_body = e.read().decode("utf-8", errors="ignore")[:200]
            except Exception:
                pass
            last_err = f"{model} HTTP {e.code}: {err_body}"
            print(f"[!] Gemini error: {last_err}", flush=True)
            time.sleep(1.5)
        except Exception as e:
            last_err = f"{model} error: {e}"
            print(f"[!] Gemini exception: {last_err}", flush=True)
            time.sleep(1.0)

    raise RuntimeError(f"All Gemini models failed. Last error: {last_err}")


def transcribe_wav_bytes(wav_bytes: bytes, rms_raw: int) -> str:
    """
    Sends WAV bytes to Gemini 3.5 Flash for speech transcription.
    Returns "[SILENCE]" if no intelligible speech is present.
    """
    # Fast-path for near-zero digital silence
    if rms_raw < 18:
        return "[SILENCE]"

    b64_audio = base64.b64encode(wav_bytes).decode("ascii")
    prompt = (
        "You are an accurate speech-to-text transcription engine for a personal wearable audio logger. "
        "Listen carefully to this audio clip. "
        "If there is NO intelligible human speech (only silence, room tone, static, clicks, or unintelligible noise), "
        "reply with ONLY the exact token: [SILENCE]\n"
        "If there IS human speech, transcribe the spoken words accurately and naturally in plain text. "
        "Do not add commentary or Introductory phrases—output ONLY the transcript or [SILENCE]."
    )
    parts = [
        {"inline_data": {"mime_type": "audio/wav", "data": b64_audio}},
        {"text": prompt},
    ]
    text, model = call_gemini(parts, temperature=0.1, timeout=45)
    cleaned = text.strip()
    upper = cleaned.upper()
    if ("[SILENCE]" in upper or "<NOISE>" in upper or "[NOISE]" in upper or "<SILENCE>" in upper) and len(cleaned) < 25:
        return "[SILENCE]"
    return cleaned


# ─── Background Worker for Audio Chunks ──────────────────────────────────────
def transcription_worker():
    print("[*] Background transcription worker started.", flush=True)
    while True:
        item = transcription_queue.get()
        if item is None:
            break
        chunk_id, session_id, wav_bytes, rms_raw = item
        try:
            transcript = transcribe_wav_bytes(wav_bytes, rms_raw)
            is_speech = transcript != "[SILENCE]" and len(transcript.strip()) > 0
            status = "SPEECH" if is_speech else "SILENCE"

            with db_lock:
                conn = get_db()
                cur = conn.cursor()
                cur.execute(
                    "UPDATE chunks SET status = ?, transcript = ? WHERE id = ?",
                    (status, transcript if is_speech else "", chunk_id),
                )
                if is_speech:
                    cur.execute(
                        "UPDATE sessions SET speech_chunks_count = speech_chunks_count + 1 WHERE session_id = ?",
                        (session_id,),
                    )
                conn.commit()
                conn.close()

            if is_speech:
                log_event(f"[✓] Chunk #{chunk_id} [SPEECH]: {transcript[:140]}")
            else:
                log_event(f"[-] Chunk #{chunk_id} [SILENCE] (rms={rms_raw})")
        except Exception as e:
            log_event(f"[!] Worker error on chunk #{chunk_id}: {e}")
            with db_lock:
                conn = get_db()
                conn.execute(
                    "UPDATE chunks SET status = ?, transcript = ? WHERE id = ?",
                    ("ERROR", f"Error: {e}", chunk_id),
                )
                conn.commit()
                conn.close()
        finally:
            transcription_queue.task_done()


# ─── Markdown to Clean Email HTML ────────────────────────────────────────────
def markdown_to_html(md_text: str) -> str:
    lines = md_text.split("\n")
    out = []
    in_list = False
    for raw_line in lines:
        line = raw_line.strip()
        if not line:
            if in_list:
                out.append("</ul>")
                in_list = False
            continue
        # Escape HTML first, then apply bold/italic
        esc = html.escape(line)
        esc = re.sub(r"\*\*(.*?)\*\*", r"<strong>\1</strong>", esc)
        esc = re.sub(r"\*(.*?)\*", r"<em>\1</em>", esc)

        if esc.startswith("### "):
            if in_list:
                out.append("</ul>")
                in_list = False
            out.append(
                f'<h3 style="color:#0f172a;font-size:16px;margin:18px 0 8px 0;'
                f'border-bottom:1px solid #e2e8f0;padding-bottom:4px;">{esc[4:]}</h3>'
            )
        elif esc.startswith("## "):
            if in_list:
                out.append("</ul>")
                in_list = False
            out.append(
                f'<h2 style="color:#1e3a8a;font-size:18px;margin:20px 0 10px 0;'
                f'border-bottom:2px solid #dbeafe;padding-bottom:6px;">{esc[3:]}</h2>'
            )
        elif esc.startswith("- ") or esc.startswith("* "):
            if not in_list:
                out.append('<ul style="margin:8px 0 12px 0;padding-left:22px;color:#334155;">')
                in_list = True
            out.append(f'<li style="margin-bottom:6px;line-height:1.6;">{esc[2:]}</li>')
        else:
            if in_list:
                out.append("</ul>")
                in_list = False
            out.append(f'<p style="margin:0 0 10px 0;line-height:1.65;color:#334155;">{esc}</p>')
    if in_list:
        out.append("</ul>")
    return "\n".join(out)


# ─── Daily Summary & Long-Term Context Builder ───────────────────────────────
def generate_and_send_daily_summary(target_date_nzst: str = None, send_email_flag: bool = True):
    # Wait up to 25s for any in-flight audio chunks to finish transcribing first
    t_wait = time.time()
    while not transcription_queue.empty() and (time.time() - t_wait < 25.0):
        time.sleep(0.5)
    time.sleep(0.5)

    dt_now = now_nz()
    if not target_date_nzst:
        target_date_nzst = dt_now.strftime("%Y-%m-%d")

    with db_lock:
        conn = get_db()
        cur = conn.cursor()

        cur.execute(
            "SELECT id, time_nzst, duration_sec, status, transcript FROM chunks WHERE date_nzst = ? ORDER BY id ASC",
            (target_date_nzst,),
        )
        rows = cur.fetchall()

        cur.execute("SELECT * FROM long_term_context WHERE id = 1")
        lt_row = cur.fetchone()
        prior_context = lt_row["context_markdown"] if lt_row else ""
        days_summarized = lt_row["days_summarized"] if lt_row else 0

        cur.execute(
            "SELECT date_nzst, summary_markdown FROM daily_summaries WHERE date_nzst < ? ORDER BY date_nzst DESC LIMIT 5",
            (target_date_nzst,),
        )
        recent_days = cur.fetchall()
        conn.close()

    total_chunks = len(rows)
    max_chunk_id = max([r["id"] for r in rows], default=0)
    speech_rows = [r for r in rows if r["status"] == "SPEECH" and r["transcript"]]
    speech_chunks = len(speech_rows)
    total_duration_min = round(sum(r["duration_sec"] or 0 for r in rows) / 60.0, 1)
    speech_duration_min = round(sum(r["duration_sec"] or 0 for r in speech_rows) / 60.0, 1)

    if speech_chunks > 0:
        transcript_log = "\n".join(
            f"[{r['time_nzst']}] ({r['duration_sec']}s) {r['transcript']}" for r in speech_rows
        )
    else:
        transcript_log = "(No intelligible speech segments were captured on this date.)"

    recent_days_text = "\n\n".join(
        f"--- {r['date_nzst']} ---\n{r['summary_markdown'][:1200]}" for r in recent_days
    ) or "None yet."

    # 1. Generate Today's Summary + Context Synthesis
    summary_prompt = f"""You are Dipesh Trikam's personal AI Chief of Staff and Memory Engine.
Today's Date (NZT): {target_date_nzst}
Recording Stats Today: {total_chunks} total audio chunks ({total_duration_min} mins monitored), {speech_chunks} speech segments ({speech_duration_min} mins of active speech).

=== EVOLVING LONG-TERM CONTEXT (Accumulated prior to today) ===
{prior_context}

=== RECENT DAILY SUMMARIES ===
{recent_days_text}

=== TODAY'S TIMESTAMPED AUDIO TRANSCRIPTS ({target_date_nzst}) ===
{transcript_log}

Write a comprehensive, insightful Daily Audio & Context Briefing using clean Markdown with these exact sections:
## Executive Summary
A clear synthesis of what Dipesh talked about, worked on, or experienced today.

## Key Conversations & Timeline Highlights
Bullet points with timestamps summarizing important topics, meetings, or thoughts captured today.

## Action Items, Commitments & Follow-Ups
Specific tasks, reminders, or ideas worth acting on (with **bold** highlights).

## Ongoing Context & Pattern Connections
How today's captured audio connects to Dipesh's broader projects, recurring themes, and historical context over time.
"""

    summary_md, model_used = call_gemini([{"text": summary_prompt}], temperature=0.3, timeout=60)

    # 2. Update Rolling Long-Term Context Memory (if speech occurred today)
    updated_context_md = prior_context
    new_days_count = days_summarized
    if speech_chunks > 0:
        context_prompt = f"""You maintain a persistent, cumulative Long-Term Memory dossier for Dipesh Trikam based on his daily audio recordings.

=== PREVIOUS LONG-TERM MEMORY DOSSIER ===
{prior_context}

=== TODAY'S SUMMARY ({target_date_nzst}) ===
{summary_md}

=== TODAY'S RAW TRANSCRIPTS ({target_date_nzst}) ===
{transcript_log}

Produce an updated, well-structured Long-Term Memory Dossier (under 1,200 words) that preserves important historical facts while integrating today's new information. Organize it with:
## Active Projects & Workstreams
## Key People, Stakeholders & Entities Mentioned
## Open Action Items & Goals
## Recurring Themes, Preferences & Chronological Milestones
"""
        try:
            updated_context_md, _ = call_gemini([{"text": context_prompt}], temperature=0.2, timeout=60)
            new_days_count = days_summarized + 1
        except Exception as e:
            print(f"[!] Context update warning: {e}", flush=True)

    summary_body_html = markdown_to_html(summary_md)
    context_body_html = markdown_to_html(updated_context_md)

    # Full chronological transcripts for the email body
    full_transcripts_html = ""
    for r in speech_rows:
        full_transcripts_html += (
            f'<tr>'
            f'<td style="padding:8px 12px;border-bottom:1px solid #f1f5f9;color:#475569;font-family:monospace;white-space:nowrap;vertical-align:top;background:#f8fafc;width:95px;">'
            f'<strong>{html.escape(r["time_nzst"])}</strong><br><span style="font-size:10px;color:#94a3b8;">{r["duration_sec"]}s</span></td>'
            f'<td style="padding:8px 12px;border-bottom:1px solid #f1f5f9;color:#0f172a;line-height:1.55;">'
            f'{html.escape(r["transcript"])}</td>'
            f'</tr>'
        )
    if not full_transcripts_html:
        full_transcripts_html = '<tr><td colspan="2" style="padding:14px;color:#94a3b8;text-align:center;">No speech transcripts recorded today.</td></tr>'

    pretty_date = dt_now.strftime("%A, %d %B %Y")
    email_html = f"""<!DOCTYPE html>
<html>
<head><meta charset="utf-8"></head>
<body style="margin:0;padding:20px;background:#f1f5f9;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;">
  <table width="100%" cellpadding="0" cellspacing="0" style="max-width:680px;margin:0 auto;background:#ffffff;border-radius:12px;overflow:hidden;box-shadow:0 4px 12px rgba(0,0,0,0.06);">
    <tr>
      <td style="background:linear-gradient(135deg,#0f172a 0%,#1e3a8a 100%);padding:26px 30px;color:#ffffff;">
        <div style="font-size:12px;text-transform:uppercase;letter-spacing:1.2px;color:#93c5fd;font-weight:600;">M5StickC Plus • Personal Audio Memory</div>
        <h1 style="margin:6px 0 4px 0;font-size:22px;font-weight:700;">🎙️ Daily Audio &amp; Context Briefing</h1>
        <div style="font-size:13px;color:#cbd5e1;">{pretty_date} &nbsp;•&nbsp; Model: {model_used}</div>
      </td>
    </tr>
    <tr>
      <td style="padding:20px 30px 10px 30px;">
        <table width="100%" cellpadding="0" cellspacing="0">
          <tr>
            <td style="background:#f8fafc;border:1px solid #e2e8f0;border-radius:8px;padding:12px;text-align:center;width:31%;">
              <div style="font-size:20px;font-weight:700;color:#0f172a;">{total_duration_min}m</div>
              <div style="font-size:11px;color:#64748b;margin-top:2px;">Audio Monitored</div>
            </td>
            <td width="3%"></td>
            <td style="background:#eff6ff;border:1px solid #bfdbfe;border-radius:8px;padding:12px;text-align:center;width:31%;">
              <div style="font-size:20px;font-weight:700;color:#1d4ed8;">{speech_chunks}</div>
              <div style="font-size:11px;color:#1e40af;margin-top:2px;">Speech Segments ({speech_duration_min}m)</div>
            </td>
            <td width="3%"></td>
            <td style="background:#f0fdf4;border:1px solid #bbf7d0;border-radius:8px;padding:12px;text-align:center;width:31%;">
              <div style="font-size:20px;font-weight:700;color:#047857;">Day {max(1, new_days_count)}</div>
              <div style="font-size:11px;color:#065f46;margin-top:2px;">Cumulative Context</div>
            </td>
          </tr>
        </table>
      </td>
    </tr>
    <tr>
      <td style="padding:10px 30px 20px 30px;">
        {summary_body_html}
      </td>
    </tr>
    <tr>
      <td style="padding:0 30px 24px 30px;">
        <h2 style="font-size:16px;color:#0f172a;border-bottom:2px solid #e2e8f0;padding-bottom:6px;margin:0 0 12px 0;">
          📝 Complete Verbatim Transcript ({target_date_nzst})
        </h2>
        <table width="100%" cellpadding="0" cellspacing="0" style="border:1px solid #e2e8f0;border-radius:8px;overflow:hidden;font-size:13px;">
          {full_transcripts_html}
        </table>
      </td>
    </tr>
    <tr>
      <td style="padding:0 30px 24px 30px;">
        <div style="background:#f8fafc;border-left:4px solid #0284c7;border-radius:8px;padding:18px;">
          <div style="font-size:13px;font-weight:700;text-transform:uppercase;color:#0369a1;letter-spacing:0.6px;margin-bottom:8px;">
            🧠 Evolving Long-Term Memory Bank
          </div>
          {context_body_html}
        </div>
      </td>
    </tr>
    <tr>
      <td style="background:#f8fafc;padding:14px 30px;text-align:center;font-size:11px;color:#94a3b8;border-top:1px solid #e2e8f0;">
        M5StickC Plus Audio Logger &nbsp;•&nbsp; Self-Hosted Cloud Audio Brain
      </td>
    </tr>
  </table>
</body>
</html>"""

    email_sent = 0
    if send_email_flag and SENDER_PASSWORD and SENDER_EMAIL and RECIPIENT_EMAIL:
        try:
            msg = MIMEMultipart("mixed")
            msg["Subject"] = f"🎙️ Daily Audio & Context Summary – {dt_now.strftime('%d %b %Y')} ({speech_chunks} segments)"
            msg["From"] = SENDER_EMAIL
            msg["To"] = RECIPIENT_EMAIL

            alt_part = MIMEMultipart("alternative")
            plain_full = (
                f"DAILY AUDIO & CONTEXT SUMMARY ({target_date_nzst})\n"
                f"=================================================\n\n"
                f"{summary_md}\n\n"
                f"=== COMPLETE VERBATIM TRANSCRIPT ({target_date_nzst}) ===\n"
                f"{transcript_log}\n\n"
                f"=== EVOLVING LONG-TERM MEMORY ===\n"
                f"{updated_context_md}\n"
            )
            alt_part.attach(MIMEText(plain_full, "plain", "utf-8"))
            alt_part.attach(MIMEText(email_html, "html", "utf-8"))
            msg.attach(alt_part)

            # Also attach the complete verbatim transcript as a clean downloadable .txt file
            attachment = MIMEText(plain_full, "plain", "utf-8")
            attachment.add_header(
                "Content-Disposition",
                f'attachment; filename="transcript_{target_date_nzst}.txt"',
            )
            msg.attach(attachment)

            server = smtplib.SMTP(SMTP_HOST, SMTP_PORT, timeout=25)
            server.starttls()
            server.login(SENDER_EMAIL, SENDER_PASSWORD)
            server.sendmail(SENDER_EMAIL, RECIPIENT_EMAIL, msg.as_string())
            server.quit()
            email_sent = 1
            log_event(f"[✓] Daily summary + full transcript email sent to {RECIPIENT_EMAIL}")
        except Exception as e:
            log_event(f"[!] Email send error: {e}")

    with db_lock:
        conn = get_db()
        cur = conn.cursor()
        cur.execute(
            """
            INSERT OR REPLACE INTO daily_summaries
            (date_nzst, generated_at_nzst, total_chunks, speech_chunks, speech_minutes, summary_markdown, summary_html, email_sent, last_emailed_chunk_id)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                target_date_nzst,
                dt_now.strftime("%Y-%m-%d %H:%M:%S"),
                total_chunks,
                speech_chunks,
                speech_duration_min,
                summary_md,
                email_html,
                email_sent,
                max_chunk_id if email_sent else 0,
            ),
        )
        cur.execute(
            """
            UPDATE long_term_context
            SET updated_at_nzst = ?, days_summarized = ?, context_markdown = ?
            WHERE id = 1
            """,
            (dt_now.strftime("%Y-%m-%d %H:%M:%S"), new_days_count, updated_context_md),
        )
        conn.commit()
        conn.close()

    return {
        "ok": True,
        "date_nzst": target_date_nzst,
        "total_chunks": total_chunks,
        "speech_chunks": speech_chunks,
        "email_sent": bool(email_sent),
        "recipient": RECIPIENT_EMAIL,
    }


# ─── Daily Scheduler Thread (Auto-emails at 21:00 NZT only if new recordings) ─
def daily_scheduler_loop():
    last_checked_date = None
    while True:
        try:
            dt = now_nz()
            today_str = dt.strftime("%Y-%m-%d")
            if dt.hour == 21 and dt.minute < 15 and last_checked_date != today_str:
                with db_lock:
                    conn = get_db()
                    cur = conn.cursor()
                    cur.execute(
                        """
                        SELECT COUNT(*) AS total,
                               SUM(CASE WHEN status='SPEECH' THEN 1 ELSE 0 END) AS speech,
                               COALESCE(MAX(id), 0) AS max_id
                        FROM chunks
                        WHERE date_nzst = ?
                        """,
                        (today_str,),
                    )
                    c_row = cur.fetchone()
                    total_today = c_row["total"] or 0
                    speech_today = c_row["speech"] or 0
                    max_chunk_id = c_row["max_id"] or 0

                    cur.execute(
                        "SELECT email_sent, COALESCE(last_emailed_chunk_id, 0) AS last_id FROM daily_summaries WHERE date_nzst = ?",
                        (today_str,),
                    )
                    s_row = cur.fetchone()
                    already_emailed = (s_row["email_sent"] == 1) if s_row else False
                    last_emailed_id = s_row["last_id"] if s_row else 0
                    conn.close()

                if total_today == 0 or speech_today == 0:
                    log_event(f"[SCHEDULER] 21:00 NZT ({today_str}): No speech recordings today — skipping daily email.")
                elif already_emailed and max_chunk_id <= last_emailed_id:
                    log_event(
                        f"[SCHEDULER] 21:00 NZT ({today_str}): Summary already emailed earlier today (up to chunk #{last_emailed_id}) and no new recordings since — skipping duplicate 9pm email."
                    )
                else:
                    log_event(
                        f"[SCHEDULER] 21:00 NZT ({today_str}): New recordings detected (chunk #{max_chunk_id} > #{last_emailed_id}). Generating & sending daily summary email..."
                    )
                    generate_and_send_daily_summary(today_str, send_email_flag=True)

                last_checked_date = today_str
        except Exception as e:
            log_event(f"[!] Scheduler loop error: {e}")
        time.sleep(30)


# ─── HTTP Server Handler ─────────────────────────────────────────────────────
class AudioLoggerHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        log_event(f"HTTP {self.address_string()} - {fmt % args}")

    def _send_json(self, code: int, payload: dict):
        try:
            raw = json.dumps(payload).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
            self.send_header("Access-Control-Allow-Headers", "Content-Type, Accept, Authorization, X-Device-Token")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _send_auth_required(self):
        raw = b"Authentication required"
        self.send_response(401)
        self.send_header("WWW-Authenticate", 'Basic realm="M5StickC Plus Audio Logger"')
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type, Accept, Authorization, X-Device-Token")
        self.end_headers()

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/api/ping":
            dt = now_nz()
            today_str = dt.strftime("%Y-%m-%d")
            qs = urllib.parse.parse_qs(parsed.query)
            token = self.headers.get("X-Device-Token", "")
            if is_valid_device_token(token) and ("rec" in qs or "vbat" in qs):
                device_telemetry["recording"] = qs.get("rec", ["0"])[0] == "1"
                if "vbat" in qs:
                    device_telemetry["vbat"] = re.sub(r"[^0-9.]", "", qs["vbat"][0])[:8] + "V"
                if "usb" in qs:
                    device_telemetry["usb"] = qs["usb"][0] == "1"
                if "ip" in qs and qs["ip"][0]:
                    device_telemetry["ip"] = re.sub(r"[^0-9.]", "", qs["ip"][0])[:32]
                if "ssid" in qs and qs["ssid"][0]:
                    device_telemetry["ssid"] = qs["ssid"][0].replace("\r", "").replace("\n", "")[:64]
                device_telemetry["last_seen_nzst"] = dt.strftime("%Y-%m-%d %H:%M:%S")
                device_telemetry["last_seen_epoch"] = time.time()
            elif not check_dashboard_auth(self.headers):
                self._send_auth_required()
                return

            with db_lock:
                conn = get_db()
                cur = conn.cursor()
                cur.execute(
                    "SELECT COUNT(*) AS total, SUM(CASE WHEN status='SPEECH' THEN 1 ELSE 0 END) AS speech FROM chunks WHERE date_nzst = ?",
                    (today_str,),
                )
                row = cur.fetchone()
                conn.close()
            self._send_json(
                200,
                {
                    "ok": True,
                    "time_nzst": dt.strftime("%H:%M:%S"),
                    "date_nzst": today_str,
                    "chunks_today": row["total"] or 0,
                    "speech_today": row["speech"] or 0,
                    "queue_size": transcription_queue.qsize(),
                },
            )
            return

        if not check_dashboard_auth(self.headers):
            self._send_auth_required()
            return

        if parsed.path == "/api/logs":
            lines = []
            if os.path.exists(LOG_FILE_PATH):
                with open(LOG_FILE_PATH, "r", encoding="utf-8", errors="replace") as lf:
                    lines = [l.rstrip("\n") for l in lf.readlines()[-150:]]
            device_lines = []
            if os.path.exists(DEVICE_LOG_FILE_PATH):
                with open(DEVICE_LOG_FILE_PATH, "r", encoding="utf-8", errors="replace") as df:
                    device_lines = [l.rstrip("\n") for l in df.readlines()[-150:]]
            self._send_json(200, {
                "ok": True,
                "log_file": os.path.basename(LOG_FILE_PATH),
                "lines": lines,
                "device_log_file": os.path.basename(DEVICE_LOG_FILE_PATH),
                "device_lines": device_lines,
            })
            return

        if parsed.path == "/api/status":
            dt = now_nz()
            today_str = dt.strftime("%Y-%m-%d")
            with db_lock:
                conn = get_db()
                cur = conn.cursor()
                cur.execute(
                    "SELECT id, session_id, date_nzst, time_nzst, duration_sec, rms_raw, status, transcript FROM chunks ORDER BY id DESC LIMIT 40"
                )
                chunks = [dict(r) for r in cur.fetchall()]
                cur.execute("SELECT * FROM long_term_context WHERE id = 1")
                lt = dict(cur.fetchone())
                cur.execute("SELECT date_nzst, generated_at_nzst, total_chunks, speech_chunks, speech_minutes, email_sent FROM daily_summaries ORDER BY date_nzst DESC LIMIT 10")
                summaries = [dict(r) for r in cur.fetchall()]
                conn.close()
            self._send_json(
                200,
                {
                    "ok": True,
                    "now_nzst": dt.strftime("%Y-%m-%d %H:%M:%S"),
                    "device_telemetry": device_telemetry,
                    "recent_chunks": chunks,
                    "long_term_context": lt,
                    "daily_summaries": summaries,
                },
            )
            return

        # Dynamic Plain-Text Log Routes (/logs/audio_brain.log and /logs/m5stick_device.log)
        if parsed.path in ("/logs/audio_brain.log", "/logs/m5stick_device.log"):
            target_log = LOG_FILE_PATH if parsed.path.endswith("audio_brain.log") else DEVICE_LOG_FILE_PATH
            content = ""
            if os.path.exists(target_log):
                with open(target_log, "r", encoding="utf-8", errors="replace") as f:
                    content = f.read()
            raw = content.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)
            return

        # Static Visual Guides & Markdown Docs Routes (/images/*, /docs/*, /manifest.json, /apple-touch-icon.png, /favicon.ico)
        docs_dir = os.path.join(BASE_DIR, "docs")
        if parsed.path == "/manifest.json":
            fpath = os.path.join(docs_dir, "manifest.json")
            if os.path.isfile(fpath):
                with open(fpath, "rb") as f:
                    raw = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "application/manifest+json; charset=utf-8")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)
                return

        if parsed.path in ("/apple-touch-icon.png", "/apple-touch-icon-precomposed.png", "/favicon.ico"):
            icon_name = "favicon.png" if parsed.path == "/favicon.ico" else "apple_touch_icon.png"
            fpath = os.path.join(docs_dir, "images", icon_name)
            if os.path.isfile(fpath):
                with open(fpath, "rb") as f:
                    raw = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "image/png")
                self.send_header("Cache-Control", "public, max-age=3600")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)
                return

        if parsed.path.startswith("/images/"):
            fname = os.path.basename(parsed.path)
            fpath = os.path.join(docs_dir, "images", fname)
            if os.path.isfile(fpath):
                ctype = "image/png" if fname.lower().endswith(".png") else "image/jpeg"
                with open(fpath, "rb") as f:
                    raw = f.read()
                self.send_response(200)
                self.send_header("Content-Type", ctype)
                self.send_header("Cache-Control", "public, max-age=300")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)
                return

        if parsed.path.startswith("/docs/"):
            fname = os.path.basename(parsed.path)
            fpath = os.path.join(docs_dir, fname)
            if os.path.isfile(fpath):
                ctype = "text/html; charset=utf-8" if fname.lower().endswith(".html") else "text/plain; charset=utf-8"
                with open(fpath, "rb") as f:
                    raw = f.read()
                self.send_response(200)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)
                return

        # Serve Rich Control Center & Visual Portal at / (or /portal, /index.html)
        portal_html_path = os.path.join(docs_dir, "index.html")
        if parsed.path in ("/", "/portal", "/index.html") and os.path.isfile(portal_html_path):
            with open(portal_html_path, "rb") as f:
                raw = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)
            return

        # Serve Classic Live Web Dashboard (/classic or fallback)
        dt = now_nz()
        today_str = dt.strftime("%Y-%m-%d")
        with db_lock:
            conn = get_db()
            cur = conn.cursor()
            cur.execute(
                "SELECT id, date_nzst, time_nzst, duration_sec, rms_raw, status, transcript FROM chunks ORDER BY id DESC LIMIT 30"
            )
            chunks = cur.fetchall()
            cur.execute("SELECT * FROM long_term_context WHERE id = 1")
            lt = cur.fetchone()
            conn.close()

        rows_html = ""
        for c in chunks:
            badge_bg = "#dcfce7" if c["status"] == "SPEECH" else ("#fef9c3" if c["status"] == "PENDING" else "#f1f5f9")
            badge_fg = "#166534" if c["status"] == "SPEECH" else ("#854d0e" if c["status"] == "PENDING" else "#64748b")
            txt = html.escape(c["transcript"] or ("(Processing with Gemini...)" if c["status"] == "PENDING" else "(Silence)"))
            rows_html += f"""
            <tr>
              <td style="padding:10px 12px;border-bottom:1px solid #e2e8f0;font-family:monospace;color:#475569;">#{c['id']}</td>
              <td style="padding:10px 12px;border-bottom:1px solid #e2e8f0;font-family:monospace;white-space:nowrap;">{c['date_nzst']} {c['time_nzst']}</td>
              <td style="padding:10px 12px;border-bottom:1px solid #e2e8f0;">{c['duration_sec']}s (RMS {c['rms_raw']})</td>
              <td style="padding:10px 12px;border-bottom:1px solid #e2e8f0;">
                <span style="background:{badge_bg};color:{badge_fg};padding:3px 8px;border-radius:99px;font-size:11px;font-weight:700;">{c['status']}</span>
              </td>
              <td style="padding:10px 12px;border-bottom:1px solid #e2e8f0;color:#0f172a;">{txt}</td>
            </tr>"""
        if not rows_html:
            rows_html = '<tr><td colspan="5" style="padding:20px;text-align:center;color:#94a3b8;">No audio chunks received yet. Press the M5 button on your M5StickC Plus to start recording!</td></tr>'

        lt_html = markdown_to_html(lt["context_markdown"]) if lt else ""

        page = f"""<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
  <title>M5StickC Plus — Audio Logger &amp; Memory Engine</title>
  <meta http-equiv="refresh" content="10">
  <style>
    *, *::before, *::after {{ box-sizing: border-box; }}
    html, body {{ max-width: 100%; overflow-x: clip; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; background: #0f172a; color: #f8fafc; margin: 0; padding: 16px; overflow-wrap: anywhere; }}
    .container {{ max-width: 1000px; margin: 0 auto; }}
    .header-bar {{ display: flex; flex-wrap: wrap; justify-content: space-between; align-items: center; gap: 12px; margin-bottom: 18px; }}
    .card {{ background: #ffffff; color: #0f172a; border-radius: 12px; padding: 18px; margin-bottom: 18px; box-shadow: 0 4px 16px rgba(0,0,0,0.2); overflow-x: auto; }}
    .btn {{ background: #2563eb; color: #fff; border: none; padding: 10px 16px; border-radius: 8px; font-weight: 600; cursor: pointer; font-size: 14px; min-height: 44px; }}
    .btn:hover {{ background: #1d4ed8; }}
    @media (max-width: 640px) {{
      html, body {{ padding: 10px; }}
      .card {{ padding: 14px; }}
      .btn {{ width: 100%; }}
      .header-bar form {{ width: 100%; }}
    }}
  </style>
</head>
<body>
  <div class="container">
    <div class="header-bar">
      <div>
        <div style="color:#60a5fa;font-size:12px;font-weight:700;text-transform:uppercase;letter-spacing:1px;">Self-Hosted Audio Brain • Live Hub</div>
        <h1 style="margin:4px 0 0 0;font-size:clamp(1.25rem, 2.5vw, 1.65rem);">🎙️ M5StickC Plus Audio Logger &amp; Memory Engine</h1>
      </div>
      <form method="POST" action="/api/summarize">
        <button class="btn" type="submit">✉️ Summarize &amp; Email Now</button>
      </form>
    </div>

    <div class="card">
      <h2 style="margin-top:0;font-size:18px;color:#1e3a8a;">🧠 Evolving Long-Term Context Memory (Updated: {html.escape(lt['updated_at_nzst'] or 'Pending')})</h2>
      {lt_html}
    </div>

    <div class="card">
      <h2 style="margin-top:0;font-size:18px;color:#0f172a;">📡 Live Incoming Audio Chunks &amp; Gemini Transcripts (Auto-refreshes every 10s)</h2>
      <div style="overflow-x:auto;">
        <table width="100%" cellspacing="0" cellpadding="0" style="font-size:13.5px;min-width:520px;">
          <tr style="background:#f8fafc;color:#475569;text-align:left;font-size:11px;text-transform:uppercase;">
            <th style="padding:10px 12px;">ID</th>
            <th style="padding:10px 12px;">NZST Time</th>
            <th style="padding:10px 12px;">Clip</th>
            <th style="padding:10px 12px;">State</th>
            <th style="padding:10px 12px;">Transcript</th>
          </tr>
          {rows_html}
        </table>
      </div>
    </div>
  </div>
</body>
</html>"""
        raw = page.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_POST(self):
        global last_web_summary_epoch
        parsed = urllib.parse.urlparse(self.path)

        if parsed.path == "/api/device-log":
            token = self.headers.get("X-Device-Token", "")
            if not is_valid_device_token(token):
                self._send_json(401, {"ok": False, "error": "Unauthorized"})
                return
            content_len = min(int(self.headers.get("Content-Length", "0") or "0"), 65536)
            body = self.rfile.read(content_len).decode("utf-8", errors="replace").strip()
            if body:
                clean_lines = [ln.replace("\r", "").replace("\x00", "") for ln in body.splitlines() if ln.strip()]
                if clean_lines:
                    with open(DEVICE_LOG_FILE_PATH, "a", encoding="utf-8") as df:
                        for line in clean_lines:
                            df.write(line + "\n")
                    last_line = clean_lines[-1]
                    log_event(f"[DEVICE_LOG] {last_line}")
                    device_telemetry["last_seen_nzst"] = now_nz().strftime("%Y-%m-%d %H:%M:%S")
                    device_telemetry["last_seen_epoch"] = time.time()
                    m_wifi = re.search(r"SSID='([^']+)'\s+IP=([0-9.]+)", last_line)
                    if m_wifi:
                        device_telemetry["ssid"] = m_wifi.group(1)
                        device_telemetry["ip"] = m_wifi.group(2)
                    if "RECORD_START:" in last_line or "CHUNK_OK:" in last_line:
                        device_telemetry["recording"] = True
                    elif "RECORD_STOP:" in last_line or "WIFI_CONNECTED:" in last_line or "BOOT:" in last_line:
                        device_telemetry["recording"] = False
            self._send_json(200, {"ok": True})
            return

        if parsed.path == "/api/summarize":
            try:
                token = self.headers.get("X-Device-Token", "")
                if is_valid_device_token(token):
                    # Triggered from M5StickC Plus physical Side Button: run asynchronously so device never times out
                    threading.Thread(
                        target=generate_and_send_daily_summary,
                        kwargs={"send_email_flag": True},
                        daemon=True,
                    ).start()
                    self._send_json(200, {"ok": True, "email_sent": True, "queued": True})
                    return
                if not check_dashboard_auth(self.headers):
                    self._send_auth_required()
                    return
                now_ep = time.time()
                if now_ep - last_web_summary_epoch < 60.0:
                    self._send_json(429, {"ok": False, "error": "Please wait 60 seconds between manual summary requests"})
                    return
                last_web_summary_epoch = now_ep
                result = generate_and_send_daily_summary(send_email_flag=True)
                accept = self.headers.get("Accept", "")
                if "text/html" in accept:
                    self.send_response(303)
                    self.send_header("Location", "/")
                    self.end_headers()
                else:
                    self._send_json(200, result)
            except Exception as e:
                self._send_json(500, {"ok": False, "error": str(e)})
            return

        if parsed.path == "/api/audio":
            token = self.headers.get("X-Device-Token", "")
            if not is_valid_device_token(token):
                self._send_json(401, {"ok": False, "error": "Unauthorized device token"})
                return
            raw_sid = self.headers.get("X-Session-Id", "default")
            session_id = re.sub(r"[^a-zA-Z0-9._-]", "_", raw_sid)[:48] or "default"
            if not session_id.startswith("offline_"):
                device_telemetry["recording"] = True
            device_telemetry["last_seen_nzst"] = now_nz().strftime("%Y-%m-%d %H:%M:%S")
            device_telemetry["last_seen_epoch"] = time.time()

            content_len = int(self.headers.get("Content-Length", "0"))
            if content_len <= 0 or content_len > 15 * 1024 * 1024:
                self._send_json(400, {"ok": False, "error": f"Invalid Content-Length: {content_len}"})
                return

            sample_rate = int(self.headers.get("X-Sample-Rate", "16000"))
            audio_format = self.headers.get("X-Audio-Format", "pcm16")

            # Read stream in chunks until content_len or client closes connection
            raw_payload = bytearray()
            remaining = content_len
            while remaining > 0:
                chunk = self.rfile.read(min(65536, remaining))
                if not chunk:
                    break
                raw_payload.extend(chunk)
                remaining -= len(chunk)

            if audio_format == "adpcm8k":
                raw_pcm = decode_adpcm_to_pcm16(bytes(raw_payload))
                sample_rate = 8000
                log_event(f"[OFFLOAD] Decoded offline ADPCM clip ({len(raw_payload)} B -> {len(raw_pcm)} B PCM16 at 8kHz)")
            else:
                raw_pcm = bytes(raw_payload)

            if len(raw_pcm) < 1600:
                self._send_json(200, {"ok": True, "skipped": True, "reason": "Chunk too short"})
                return

            wav_bytes, duration_sec, rms_raw, rms_norm = pcm16_to_normalized_wav(raw_pcm, sample_rate)
            dt_nz = now_nz()
            date_nzst = dt_nz.strftime("%Y-%m-%d")
            time_nzst = dt_nz.strftime("%H:%M:%S")
            utc_str = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

            # Save latest audio file for inspection/debugging (keep only last 10 files so disk stays tiny)
            wav_filename = f"chunk_{dt_nz.strftime('%Y%m%d_%H%M%S')}.wav"
            wav_path = os.path.join(AUDIO_TMP_DIR, wav_filename)
            try:
                with open(wav_path, "wb") as wf:
                    wf.write(wav_bytes)
                files = sorted(os.listdir(AUDIO_TMP_DIR))
                while len(files) > 10:
                    old = files.pop(0)
                    try:
                        os.remove(os.path.join(AUDIO_TMP_DIR, old))
                    except Exception:
                        pass
            except Exception:
                pass

            with db_lock:
                conn = get_db()
                cur = conn.cursor()
                cur.execute(
                    """
                    INSERT INTO sessions (session_id, started_at_nzst, last_chunk_at_nzst, chunks_count, speech_chunks_count, total_seconds)
                    VALUES (?, ?, ?, 1, 0, ?)
                    ON CONFLICT(session_id) DO UPDATE SET
                        last_chunk_at_nzst = excluded.last_chunk_at_nzst,
                        chunks_count = chunks_count + 1,
                        total_seconds = total_seconds + excluded.total_seconds
                    """,
                    (session_id, f"{date_nzst} {time_nzst}", f"{date_nzst} {time_nzst}", duration_sec),
                )
                cur.execute(
                    """
                    INSERT INTO chunks (session_id, date_nzst, time_nzst, created_at_utc, duration_sec, rms_raw, rms_norm, status, transcript, wav_path)
                    VALUES (?, ?, ?, ?, ?, ?, ?, 'PENDING', '', ?)
                    """,
                    (session_id, date_nzst, time_nzst, utc_str, duration_sec, rms_raw, rms_norm, wav_path),
                )
                chunk_id = cur.lastrowid
                conn.commit()
                conn.close()

            transcription_queue.put((chunk_id, session_id, wav_bytes, rms_raw))
            self._send_json(
                200,
                {
                    "ok": True,
                    "chunk_id": chunk_id,
                    "duration_sec": duration_sec,
                    "rms": rms_raw,
                },
            )
            return

        self._send_json(404, {"ok": False, "error": "Not found"})


if __name__ == "__main__":
    init_db()
    if not DEVICE_TOKEN:
        print("[!] WARNING: DEVICE_TOKEN is empty! Authenticated upload/log endpoints will reject requests with 401 until configured in .env or env vars.", flush=True)
    t_worker = threading.Thread(target=transcription_worker, daemon=True)
    t_worker.start()
    t_sched = threading.Thread(target=daily_scheduler_loop, daemon=True)
    t_sched.start()
    print(f"[*] M5StickC Plus Audio Brain listening on {HOST}:{PORT} ({TIMEZONE_NAME}: {now_nz().strftime('%Y-%m-%d %H:%M:%S')})", flush=True)
    server = ThreadingHTTPServer((HOST, PORT), AudioLoggerHandler)
    server.serve_forever()
