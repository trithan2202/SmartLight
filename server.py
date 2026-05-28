import torch
from transformers import AutoModelForSpeechSeq2Seq, AutoProcessor, pipeline as hf_pipeline
from flask import Flask, request, jsonify, send_file
from flask_cors import CORS
from concurrent.futures import ThreadPoolExecutor, as_completed
import requests, os, time, unicodedata, threading

app = Flask(__name__)
CORS(app)

#  CẤU HÌNH —
BLYNK_AUTH_TOKEN = "HMpZxfaxXqZ7hkIjBKVCRd-L30iJYhVV"
BLYNK_BASE_URL   = "https://blynk.cloud/external/api"

# Ngrok Auth Token — lấy tại: https://dashboard.ngrok.com/get-started/your-authtoken
NGROK_AUTH_TOKEN = "3DNTVIF0HhtCxamcVxYPIeQjUVc_3tafNh9otS6ogyZ2HQnvM"

FLASK_PORT = 5000

# 
#  LOAD MODEL
MODEL_ID = "doof-ferb/whisper-tiny-vi"

print(f"[Whisper] Đang tải model: {MODEL_ID}")
print("[Whisper] Lần đầu chạy sẽ tải ~500MB, vui lòng chờ...")

device      = "cuda" if torch.cuda.is_available() else "cpu"
torch_dtype = torch.float16 if torch.cuda.is_available() else torch.float32

processor = AutoProcessor.from_pretrained(MODEL_ID)
hf_model  = AutoModelForSpeechSeq2Seq.from_pretrained(
    MODEL_ID,
    torch_dtype       = torch_dtype,
    low_cpu_mem_usage = True,
)
hf_model.to(device)

asr_pipe = hf_pipeline(
    "automatic-speech-recognition",
    model             = hf_model,
    tokenizer         = processor.tokenizer,
    feature_extractor = processor.feature_extractor,
    torch_dtype       = torch_dtype,
    device            = device,
    generate_kwargs   = {
        "language"  : "vietnamese",
        "task"      : "transcribe",
        "prompt_ids": processor.get_prompt_ids(
            "bật đèn, tắt đèn, phòng khách, phòng ngủ, nhà bếp, tất cả",
            return_tensors="pt"
        ).to(device),
    },
)

print(f"[Whisper] ✅ Model sẵn sàng! Device: {device.upper()}")
print(f"[Whisper] Tốc độ dự kiến: {'~1-2s (GPU)' if device == 'cuda' else '~4-6s (CPU)'}")

# ============================================================
#  CẤU HÌNH LỆNH
# ============================================================
LENH_BAT = [
    "bật", "bặt", "bạt", "bắt",
    "mở", "on", "open", "sáng",
    "bách", "bạch",
]

LENH_TAT = [
    "tắt", "tặt", "tạt", "tắc", "tát",
    "đóng", "off", "close", "tối",
]

LENH_TU_DONG = ["tự động", "cảm biến", "auto", "bình thường", "mặc định", "trả lại"]

DEN_CONFIG = [
    {
        "pin"     : "V0",
        "ten"     : "Đèn phòng ngủ",
        "keywords": ["phòng ngủ", "phòng ngù", "phòng ngụ", "phong ngu", "ngủ", "ngù", "ngụ"],
        "kw_nodau": ["phong ngu", "ngu"],
    },
    {
        "pin"     : "V4",
        "ten"     : "Đèn hành lang",
        "keywords": ["hành lang", "hanh lang", "hành", "lạng"],
        "kw_nodau": ["hanh lang", "hanh"],
    },
    {
        "pin"     : "V5",
        "ten"     : "Đèn sân vườn",
        "keywords": ["sân vườn", "san vuon", "sân", "vườn", "ngoài sân"],
        "kw_nodau": ["san vuon", "san", "vuon"],
    },
]

ALL_PINS = [d["pin"] for d in DEN_CONFIG]

KEYWORDS_TAT_CA = [
    "tất cả", "tất thảy", "toàn bộ", "hết thảy",
    "tất cả đèn", "toàn bộ đèn",
]

# ============================================================
#  HÀM PARSE LỆNH
# ============================================================
def bo_dau(text: str) -> str:
    nfkd = unicodedata.normalize("NFKD", text)
    return "".join(c for c in nfkd if not unicodedata.combining(c)).lower()


def detect_action(t_lower: str):
    score_on  = sum(1 for tu in LENH_BAT if tu in t_lower)
    score_off = sum(1 for tu in LENH_TAT if tu in t_lower)
    score_auto = sum(1 for tu in LENH_TU_DONG if tu in t_lower)
    print(f"[Action] Score ON={score_on} | OFF={score_off}")
    if score_on == 0 and score_off == 0 and score_auto == 0:
        return None
    # return "off" if score_off >= score_on else "on" # củ: chỉ so sánh on/off
    if score_auto >= score_on and score_auto >= score_off: return 2 # Auto
    elif score_on > score_off: return 1 # Bật
    else: return 0 # Tắt


def detect_phong(t_lower: str, t_nodau: str) -> list:
    for kw in KEYWORDS_TAT_CA:
        if kw in t_lower:
            print(f"[Phong] → ALL (match: '{kw}')")
            return [{"pin": "ALL", "ten": "Tất cả đèn"}]

    found = []
    for den in DEN_CONFIG:
        khop_tu = next((kw for kw in den["keywords"] if kw in t_lower), "")
        if not khop_tu:
            khop_tu = next((kw for kw in den["kw_nodau"] if kw in t_nodau), "")
        if khop_tu:
            print(f"[Phong] → {den['pin']} '{den['ten']}' (match: '{khop_tu}')")
            found.append({"pin": den["pin"], "ten": den["ten"]})
    return found


def parse_lenh(text: str) -> dict:
    t_lower = text.lower().strip()
    t_nodau = bo_dau(t_lower)
    print(f"\n[Parse] Input: '{t_lower}'")
    action  = detect_action(t_lower)
    targets = detect_phong(t_lower, t_nodau)
    print(f"[Parse] action={action} | targets={[t['pin'] for t in targets]}")
    return {"action": action, "targets": targets, "raw": text}


# ============================================================
#  BLYNK API — song song hóa, không time.sleep
# ============================================================
def blynk_set(pin: str, value: int) -> bool:
    """Set 1 pin đơn lẻ."""
    try:
        url = f"{BLYNK_BASE_URL}/update?token={BLYNK_AUTH_TOKEN}&{pin}={value}"
        res = requests.get(url, timeout=5)
        ok  = res.status_code == 200
        print(f"[Blynk] {pin}={value} → {'OK' if ok else 'LỖI ' + str(res.status_code)}")
        return ok
    except Exception as e:
        print(f"[Blynk] Exception {pin}: {e}")
        return False


def blynk_set_parallel(pins: list, value: int) -> bool:
    """
    Gửi tất cả request ĐỒNG THỜI — không đợi tuần tự, không time.sleep.

    Cũ: V0→đợi→V1→đợi→V2 = ~500ms, đèn bật lần lượt
    Mới: V0+V1+V2 cùng lúc = ~130ms, đèn bật đồng thời
    """
    if len(pins) == 1:
        return blynk_set(pins[0], value)

    t0 = time.time()
    results = {}
    with ThreadPoolExecutor(max_workers=len(pins)) as executor:
        future_map = {executor.submit(blynk_set, pin, value): pin for pin in pins}
        for future in as_completed(future_map):
            pin = future_map[future]
            try:
                results[pin] = future.result()
            except Exception as e:
                print(f"[Blynk] Thread lỗi {pin}: {e}")
                results[pin] = False

    elapsed_ms = (time.time() - t0) * 1000
    ok_count   = sum(1 for v in results.values() if v)
    print(f"[Blynk] Parallel {len(pins)} pins → {ok_count}/{len(pins)} OK ({elapsed_ms:.0f}ms)")
    return all(results.values())


def blynk_set_targets(targets: list, value: int):
    """Điều khiển danh sách đèn song song."""
    if not targets:
        return
    pins = ALL_PINS if any(t["pin"] == "ALL" for t in targets) else [t["pin"] for t in targets]
    blynk_set_parallel(pins, value)


# ============================================================
#  ENDPOINTS
# ============================================================
@app.route("/")
def index():
    return send_file("index.html")


@app.route("/transcribe", methods=["POST"])
def transcribe():
    if "audio" not in request.files:
        return jsonify({"error": "Không có file audio"}), 400

    audio_file = request.files["audio"]
    temp_path  = f"temp_{int(time.time()*1000)}.webm"

    try:
        audio_file.save(temp_path)

        t0         = time.time()
        result_asr = asr_pipe(temp_path)
        text       = result_asr["text"].strip()
        elapsed    = time.time() - t0

        print(f"[Whisper] '{text}' ({elapsed:.2f}s)")

        if not text.strip():
            return jsonify({"text": "", "success": False,
                            "message": "Không nghe thấy giọng nói."})

        lenh = parse_lenh(text)

        if lenh["action"] is None:
            return jsonify({"text": text, "success": False,
                            "message": "❓ Không nhận ra bật/tắt. Thử: 'Bật đèn phòng ngủ'"})

        if not lenh["targets"]:
            return jsonify({"text": text, "success": False,
                            "message": "❓ Không rõ phòng nào. Thử: 'Bật đèn phòng ngủ'"})

        # value = 1 if lenh["action"] == "on" else 0

        # t_blynk = time.time()
        # blynk_set_targets(lenh["targets"], value)
        # blynk_ms = round((time.time() - t_blynk) * 1000)

        value = lenh["action"] # Lấy thẳng số 0, 1, 2

        t_blynk = time.time()
        blynk_set_targets(lenh["targets"], value)
        blynk_ms = round((time.time() - t_blynk) * 1000)

        ten_list    = [t["ten"] for t in lenh["targets"]]
        ten_hienthi = " & ".join(ten_list) if len(ten_list) > 1 else ten_list[0]
        
        if value == 1: action_str = "BẬT 💡"
        elif value == 0: action_str = "TẮT 🌙"
        else: action_str = "TỰ ĐỘNG 🔄"

        ten_list    = [t["ten"] for t in lenh["targets"]]
        ten_hienthi = " & ".join(ten_list) if len(ten_list) > 1 else ten_list[0]
        action_str  = "BẬT 💡" if value == 1 else "TẮT 🌙"

        return jsonify({
            "text"         : text,
            "action"       : lenh["action"],
            "pins"         : [t["pin"] for t in lenh["targets"]],
            "ten"          : ten_hienthi,
            "success"      : True,
            "message"      : f"✅ Đã {action_str} {ten_hienthi}",
            "time_stt_s"   : round(elapsed, 2),
            "time_blynk_ms": blynk_ms,
        })

    except Exception as e:
        print(f"[Server] Lỗi: {e}")
        return jsonify({"error": str(e), "success": False}), 500
    finally:
        if os.path.exists(temp_path):
            os.remove(temp_path)


@app.route("/status")
def status():
    return jsonify({
        "status"   : "ok",
        "model"    : MODEL_ID,
        "device"   : device,
        "ngrok_url": ngrok_url if ngrok_url else "chưa kết nối",
        "den_count": len(DEN_CONFIG),
        "all_pins" : ALL_PINS,
    })


# ============================================================
#  NGROK — tự động mở tunnel HTTPS
# ============================================================
ngrok_url = None


def start_ngrok():
    """Khởi động Ngrok tunnel trong thread riêng, không block Flask."""
    global ngrok_url
    try:
        from pyngrok import ngrok, conf

        if NGROK_AUTH_TOKEN and NGROK_AUTH_TOKEN != "YOUR_NGROK_AUTH_TOKEN_HERE":
            conf.get_default().auth_token = NGROK_AUTH_TOKEN
        else:
            print("[Ngrok] ⚠️  NGROK_AUTH_TOKEN chưa được điền!")
            print("[Ngrok]     Lấy token tại: https://dashboard.ngrok.com/get-started/your-authtoken")
            return

        tunnel    = ngrok.connect(FLASK_PORT, "http")
        ngrok_url = tunnel.public_url

        # pyngrok trả về http:// → ép sang https://
        if ngrok_url.startswith("http://"):
            ngrok_url = ngrok_url.replace("http://", "https://", 1)

        print("\n" + "="*60)
        print("  ✅ NGROK TUNNEL SẴN SÀNG")
        print(f"  📱 Điện thoại mở URL: {ngrok_url}")
        print(f"  💻 Nội bộ           : http://localhost:{FLASK_PORT}")
        print("  ℹ️  HTTPS tự động → microphone hoạt động ngay")
        print("  ℹ️  Không cần cùng WiFi — demo được ở bất kỳ đâu")
        print("="*60 + "\n")

    except ImportError:
        print("[Ngrok] ❌ Chưa cài pyngrok!")
        print("[Ngrok]    Chạy: pip install pyngrok")
        print("[Ngrok] ⚠️  Server vẫn chạy HTTP nội bộ tại port", FLASK_PORT)
    except Exception as e:
        print(f"[Ngrok] ❌ Lỗi khởi động tunnel: {e}")
        if "auth" in str(e).lower() or "token" in str(e).lower():
            print("[Ngrok]    Kiểm tra lại NGROK_AUTH_TOKEN")
        elif "limit" in str(e).lower():
            print("[Ngrok]    Free plan chỉ cho 1 tunnel cùng lúc.")
            print("[Ngrok]    Đóng terminal cũ hoặc vào dashboard.ngrok.com → Agents để kill.")


# ============================================================
#  MAIN
# ============================================================
if __name__ == "__main__":
    print("\n" + "="*60)
    print("  Smart Lighting Server — Whisper-VI + Ngrok")
    print(f"  Model : {MODEL_ID}")
    print(f"  Device: {device.upper()}")
    print(f"  Đèn   : {len(DEN_CONFIG)} ({', '.join(ALL_PINS)})")
    print("="*60)

    # Khởi động Ngrok trong thread riêng
    ngrok_thread = threading.Thread(target=start_ngrok, daemon=True)
    ngrok_thread.start()
    ngrok_thread.join(timeout=8)   # đợi tối đa 8s để lấy URL

    # Flask chạy HTTP thông thường
    # Ngrok handle HTTPS bên ngoài → không cần ssl_context
    print(f"\n[Flask] Chạy tại http://localhost:{FLASK_PORT} ...")
    app.run(host="0.0.0.0", port=FLASK_PORT, debug=False, use_reloader=False)
