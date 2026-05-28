#define BLYNK_TEMPLATE_ID   "TMPL6V2ArJyR3"
#define BLYNK_TEMPLATE_NAME "SmartLightingVer2"
#define BLYNK_AUTH_TOKEN    "HMpZxfaxXqZ7hkIjBKVCRd-L30iJYhVV"
#define BLYNK_PRINT Serial

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <HTTPClient.h>
#include <time.h>

// ============================================================
//  CẤU HÌNH WiFi & TELEGRAM
// ============================================================
const char* WIFI_SSID = "Wokwi-GUEST";   
const char* WIFI_PASS = "";

const char* BOT_TOKEN = "8389784876:AAGh4vf1KudsSWAuAk9AxP6A2aPzYao_Z6k";
const char* CHAT_ID   = "-5233039664";
// ID chat : "6250626194"
// ============================================================
//  CHÂN GPIO
// ============================================================
// LED output (Wokwi: active-HIGH)
#define PIN_LED1        26    // Phòng ngủ   — Voice AI
#define PIN_LED2        27    // Hành lang   — LDR
#define PIN_LED3        14    // Sân vườn    — PIR

// Cảm biến input
#define PIN_LDR         34    // Quang trở   — ADC (0–4095)
#define PIN_PIR         35    // PIR HC-SR501 — Digital (HIGH=có người)

// Nút vật lý — mỗi đèn 1 nút riêng (INPUT_PULLUP)
#define PIN_NUT1        18    // Nút override LED 1
#define PIN_NUT2        19    // Nút override LED 2
#define PIN_NUT3        23    // Nút override LED 3

// ============================================================
//  NGƯỠNG CẢM BIẾN
// ============================================================

#define LDR_THRESHOLD   2000

// PIR: Sau khi phát hiện chuyển động, giữ đèn bật thêm bao lâu?
// Đơn vị: milliseconds. Mặc định 10 giây.
#define PIR_HOLD_MS     10000

// ============================================================
//  BIẾN TRẠNG THÁI ĐÈN
// ============================================================
bool state_led1 = false;    // LED 1: do Voice/Blynk quyết định
bool state_led2 = false;    // LED 2: do LDR quyết định (hoặc nút override)
bool state_led3 = false;    // LED 3: do PIR quyết định (hoặc nút override)

// Override thủ công từ nút vật lý
// Khi override=true: cảm biến tạm dừng, nút giữ quyền điều khiển
bool override_led2 = false;
bool override_led3 = false;

// PIR hold timer: thời điểm cuối cùng phát hiện chuyển động
unsigned long pirLastDetected = 0;
bool pirHolding = false;

// Debounce nút
bool nut1Truoc = HIGH, nut2Truoc = HIGH, nut3Truoc = HIGH;

bool daKhoiDong = false;
BlynkTimer timer;

// ============================================================
//  CẤU HÌNH THỜI GIAN THỰC (NTP)
// ============================================================
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7 * 3600; // Múi giờ Việt Nam (GMT+7)
const int   daylightOffset_sec = 0;   // Không có giờ mùa hè

String layThoiGianThuc() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    return "Time_Error";
  }
  char timeStringBuff[50];
  // Định dạng chuẩn: HH:MM:SS - DD/MM/YYYY
  strftime(timeStringBuff, sizeof(timeStringBuff), "%H:%M:%S - %d/%m/%Y", &timeinfo);
  return String(timeStringBuff);
}

// ============================================================
//  HÀM GỬI TELEGRAM (NÂNG CẤP DÙNG HTTP POST + JSON)
// ============================================================
void guiTelegram(String msg) {
  if (WiFi.status() != WL_CONNECTED) return;
  
  HTTPClient http;
  
  // URL bây giờ rất ngắn gọn, không chứa tin nhắn
  String url = "https://api.telegram.org/bot";
  url += BOT_TOKEN;
  url += "/sendMessage";
  
  http.begin(url);
  
  // Báo cho Telegram biết chúng ta gửi dữ liệu chuẩn JSON
  http.addHeader("Content-Type", "application/json");

  // Xử lý chuỗi để an toàn khi đóng gói vào JSON
  msg.replace("\"", "\\\""); // Chống lỗi nếu trong tin nhắn có dấu ngoặc kép
  msg.replace("\n", "\\n");  // Giữ nguyên định dạng xuống dòng

  // Đóng gói dữ liệu (Payload)
  String payload = "{\"chat_id\":\"" + String(CHAT_ID) + "\",\"text\":\"" + msg + "\",\"parse_mode\":\"HTML\"}";

  // Gửi bằng lệnh POST thay vì GET
  int code = http.POST(payload);
  
  Serial.printf("[Telegram] HTTP %d\n", code);
  http.end();
}

// ============================================================
//  HÀM SET LED — tập trung xử lý active-HIGH/LOW
// ============================================================
void setLED(int pin, bool batTat) {
  // Wokwi: LED active-HIGH (HIGH=sáng)
  // Thật: đổi thành  digitalWrite(pin, batTat ? LOW : HIGH)
  digitalWrite(pin, batTat ? HIGH : LOW);
}

// ============================================================
//  LED 1 — VOICE AI
//  Nguồn: server.py gọi Blynk API → BLYNK_WRITE(V0) → hàm này
//         hoặc Blynk App nhấn Switch V0
//         hoặc Nút 1 vật lý
// ============================================================
void dieuKhienLED1(bool batTat, String nguon) {
  if (batTat == state_led1) return;
  state_led1 = batTat;
  setLED(PIN_LED1, batTat);

  // Đồng bộ Blynk App
  Blynk.virtualWrite(V0, batTat ? 1 : 0);

  String trangThai = batTat ? "BAT" : "TAT";
  String log = "[" + nguon + "] Phong Ngu: " + trangThai;
  Blynk.virtualWrite(V9, log);
  Serial.println(log);

  // String msg = batTat
  //   ? "<b>💡 BẬT</b> Đèn phòng ngủ [" + nguon + "]"
  //   : "<b>🌙 TẮT</b> Đèn phòng ngủ [" + nguon + "]";
  String thoiGian = layThoiGianThuc();
  String msg = "<b>[HỆ THỐNG SMART HOME]</b>\n";
  msg += "⏱ <b>" + thoiGian + "</b>\n";
  msg += "-----------------------------------\n";
  msg += "📍 <b>Khu vực:</b> Phòng Ngủ\n";
  
  if (batTat) {
    msg += "⚙️ <b>Trạng thái:</b> Đã cấp điện (ON)\n";
    msg += "🎛️ <b>Kích hoạt bởi:</b> " + nguon + "\n";
    msg += "📝 <b>Log:</b> Hoạt động bình thường.";
  } else {
    msg += "⚙️ <b>Trạng thái:</b> Ngắt điện (OFF)\n";
    msg += "🎛️ <b>Kích hoạt bởi:</b> " + nguon + "\n";
    msg += "📝 <b>Log:</b> Đã ngắt an toàn.";
  }
  guiTelegram(msg);
}

// ============================================================
//  LED 2 — LDR TỰ ĐỘNG
//  Đọc mỗi 2 giây. Nút 2 có thể override tạm thời.
// ============================================================
void capNhatLED2() {
  if (override_led2) return;   // Nút đang override → bỏ qua cảm biến

  int ldrVal = analogRead(PIN_LDR);
  bool canBat = (ldrVal > LDR_THRESHOLD);

  // Ghi giá trị LDR lên V3 để xem trên Blynk
  Blynk.virtualWrite(V3, ldrVal);

  if (canBat == state_led2) return;   // Không thay đổi → bỏ qua
  state_led2 = canBat;
  setLED(PIN_LED2, canBat);
  Blynk.virtualWrite(V1, canBat ? 1 : 0);

  String log = canBat
    ? "LED2 Hanh Lang: BAT (LDR=" + String(ldrVal) + ")"
    : "LED2 Hanh Lang: TAT (LDR=" + String(ldrVal) + ")";
  Blynk.virtualWrite(V9, log);
  Serial.println(log);

  // String msg = canBat
  //   ? "<b>💡 BẬT</b> Đèn hành lang [LDR=" + String(ldrVal) + "]"
  //   : "<b>🌙 TẮT</b> Đèn hành lang [LDR=" + String(ldrVal) + "]";
  String thoiGian = layThoiGianThuc();
  String msg = "<b>[BÁO CÁO TỰ ĐỘNG LDR]</b>\n";
  msg += "⏱ <b>" + thoiGian + "</b>\n";
  msg += "-----------------------------------\n";
  msg += "📍 <b>Khu vực:</b> Hành Lang\n";
  
  if (canBat) {
    msg += "⚙️ <b>Trạng thái:</b> Đã bật đèn (AUTO-ON)\n";
    msg += "📊 <b>Cảm biến:</b> Độ sáng thấp (Giá trị: " + String(ldrVal) + ")\n";
    msg += "📝 <b>Ghi chú:</b> Đèn được kích hoạt tự động do trời tối.";
  } else {
    msg += "⚙️ <b>Trạng thái:</b> Tắt đèn (AUTO-OFF)\n";
    msg += "📊 <b>Cảm biến:</b> Đủ ánh sáng (Giá trị: " + String(ldrVal) + ")\n";
    msg += "📝 <b>Ghi chú:</b> Đã ngắt điện để tiết kiệm năng lượng.";
  }
  guiTelegram(msg);
}

// ============================================================
//  LED 3 — PIR TỰ ĐỘNG
//  Phát hiện chuyển động → BẬT đèn, giữ PIR_HOLD_MS sau đó TẮT.
//  Nút 3 có thể override tạm thời.
// ============================================================
void capNhatLED3() {
  if (override_led3) return;   // Nút đang override → bỏ qua cảm biến

  bool pirDetected = (digitalRead(PIN_PIR) == HIGH);
  unsigned long now = millis();

  if (pirDetected) {
    pirLastDetected = now;   // Cập nhật thời điểm phát hiện gần nhất
    pirHolding      = true;
  }

  // Giữ đèn bật thêm PIR_HOLD_MS sau lần phát hiện cuối
  bool canBat = pirHolding && (now - pirLastDetected < PIR_HOLD_MS);

  // Reset nếu hết thời gian giữ
  if (!canBat && pirHolding) {
    pirHolding = false;
  }

  if (canBat == state_led3) return;
  state_led3 = canBat;
  setLED(PIN_LED3, canBat);
  Blynk.virtualWrite(V2, canBat ? 1 : 0);

  String log = canBat
    ? "LED3 San Vuon: BAT [PIR phat hien]"
    : "LED3 San Vuon: TAT [het " + String(PIR_HOLD_MS/1000) + "s]";
  Blynk.virtualWrite(V9, log);
  Serial.println(log);

  // String msg = canBat
  //   ? "<b>🚶 PHÁT HIỆN CHUYỂN ĐỘNG</b> → BẬT đèn sân vườn"
  //   : "<b>🌙 TẮT</b> Đèn sân vườn [hết " + String(PIR_HOLD_MS/1000) + "s]";
  String thoiGian = layThoiGianThuc();
  String msg = "<b>[CẢNH BÁO AN NINH PIR]</b>\n";
  msg += "⏱ <b>" + thoiGian + "</b>\n";
  msg += "-----------------------------------\n";
  msg += "📍 <b>Khu vực:</b> Sân Vườn\n";
  
  if (canBat) {
    msg += "🚨 <b>Trạng thái:</b> BẬT ĐÈN RỌI\n";
    msg += "⚠️ <b>Sự kiện:</b> Phát hiện có vật thể di chuyển!\n";
  } else {
    msg += "⚙️ <b>Trạng thái:</b> Đã tắt rọi (Chế độ chờ)\n";
    msg += "📝 <b>Sự kiện:</b> Khu vực an toàn (hết " + String(PIR_HOLD_MS/1000) + "s)\n";
  }
  guiTelegram(msg);
}

// ============================================================
//  3 NÚT VẬT LÝ RIÊNG BIỆT
//  Mỗi nút toggle đèn tương ứng, tạm dừng cảm biến (override)
// ============================================================
void kiemTraNut() {
  unsigned long now = millis();

  // ── Nút 1 — LED 1 (Voice) ─────────────────────────────
  bool n1 = digitalRead(PIN_NUT1);
  if (nut1Truoc == HIGH && n1 == LOW) {
    dieuKhienLED1(!state_led1, "NUT1");
    delay(50);
  }
  nut1Truoc = n1;

  // ── Nút 2 — LED 2 (LDR) ───────────────────────────────
  bool n2 = digitalRead(PIN_NUT2);
  if (nut2Truoc == HIGH && n2 == LOW) {
    // Toggle override: bật lần 1 = override ON, bật lần 2 = trả về cảm biến
    override_led2 = !override_led2;

    if (override_led2) {
      // Override: toggle trạng thái đèn
      state_led2 = !state_led2;
      setLED(PIN_LED2, state_led2);
      Blynk.virtualWrite(V1, state_led2 ? 1 : 0);
      String msg = state_led2
        ? "<b>✋ NÚT 2</b>: BẬT đèn hành lang [Override LDR]"
        : "<b>✋ NÚT 2</b>: TẮT đèn hành lang [Override LDR]";
      Serial.println(msg);
      guiTelegram(msg);
    } else {
      // Trả quyền về cảm biến
      Serial.println("[NUT2] Tra quyen dieu khien ve LDR");
      guiTelegram("<b>✋ NÚT 2</b>: Trả quyền về LDR tự động");
    }
    delay(50);
  }
  nut2Truoc = n2;

  // ── Nút 3 — LED 3 (PIR) ───────────────────────────────
  bool n3 = digitalRead(PIN_NUT3);
  if (nut3Truoc == HIGH && n3 == LOW) {
    override_led3 = !override_led3;

    if (override_led3) {
      state_led3 = !state_led3;
      setLED(PIN_LED3, state_led3);
      Blynk.virtualWrite(V2, state_led3 ? 1 : 0);
      String msg = state_led3
        ? "<b>✋ NÚT 3</b>: BẬT đèn sân vườn [Override PIR]"
        : "<b>✋ NÚT 3</b>: TẮT đèn sân vườn [Override PIR]";
      Serial.println(msg);
      guiTelegram(msg);
    } else {
      pirHolding = false;   // Reset PIR timer khi trả lại cảm biến
      Serial.println("[NUT3] Tra quyen dieu khien ve PIR");
      guiTelegram("<b>✋ NÚT 3</b>: Trả quyền về PIR tự động");
    }
    delay(50);
  }
  nut3Truoc = n3;
}

// ============================================================
//  BLYNK CALLBACKS
// ============================================================
BLYNK_WRITE(V0) {
  // Voice AI hoặc Blynk App → LED 1
  int val = param.asInt();
  dieuKhienLED1(val == 1, "BLYNK");
}

// ============================================================
//  XỬ LÝ VOICE AI - ĐÈN HÀNH LANG (V4)
// ============================================================
BLYNK_WRITE(V4) {
  int lenhAI = param.asInt(); 
  
  if (lenhAI == 1 || lenhAI == 0) {
    override_led2 = true;        // AI giành quyền, khóa LDR
    bool trangThaiMoi = (lenhAI == 1);
    state_led2 = trangThaiMoi;
    setLED(PIN_LED2, trangThaiMoi);
    Blynk.virtualWrite(V1, trangThaiMoi ? 1 : 0);
    
    // Gửi Telegram
    guiTelegram("<b>[HÀNH LANG]</b>\n🤖 AI đã " + String(trangThaiMoi ? "BẬT" : "TẮT") + " đèn và tạm ngưng LDR tự động.");
  } 
  else if (lenhAI == 2) {
    override_led2 = false;       // Trả quyền cho LDR
    guiTelegram("<b>[HÀNH LANG]</b>\n🔄 Đã trả quyền điều khiển về cảm biến tự động.");
  }
}

// ============================================================
//  XỬ LÝ VOICE AI - ĐÈN SÂN VƯỜN (V5)
// ============================================================
BLYNK_WRITE(V5) {
  int lenhAI = param.asInt(); 
  
  if (lenhAI == 1 || lenhAI == 0) {
    override_led3 = true;        // AI giành quyền, khóa PIR
    bool trangThaiMoi = (lenhAI == 1);
    state_led3 = trangThaiMoi;
    setLED(PIN_LED3, trangThaiMoi);
    Blynk.virtualWrite(V2, trangThaiMoi ? 1 : 0);
    
    // Gửi Telegram
    guiTelegram("<b>[SÂN VƯỜN]</b>\n🤖 AI đã " + String(trangThaiMoi ? "BẬT" : "TẮT") + " đèn và tạm ngưng PIR tự động.");
  } 
  else if (lenhAI == 2) {
    override_led3 = false;       // Trả quyền cho PIR
    pirHolding = false;          // Đặt lại bộ đếm giờ của PIR
    guiTelegram("<b>[SÂN VƯỜN]</b>\n🔄 Đã trả quyền điều khiển về cảm biến chuyển động.");
  }
}

BLYNK_CONNECTED() {
  if (!daKhoiDong) {
    Blynk.virtualWrite(V0, 0);
    Blynk.virtualWrite(V1, 0);
    Blynk.virtualWrite(V2, 0);
    Blynk.virtualWrite(V9, "Smart Lighting v2 khoi dong");
    daKhoiDong = true;
    Serial.println("[Blynk] Reset tat ca ve TAT");
  } else {
    Blynk.syncVirtual(V0);   // Chỉ sync LED 1 (Voice)
    Serial.println("[Blynk] Reconnect: sync V0");
  }
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Smart Lighting v2 ===");
  Serial.println("LED1: Voice+Nut1 | LED2: LDR+Nut2 | LED3: PIR+Nut3");

  // LED output
  pinMode(PIN_LED1, OUTPUT);
  pinMode(PIN_LED2, OUTPUT);
  pinMode(PIN_LED3, OUTPUT);
  setLED(PIN_LED1, false);
  setLED(PIN_LED2, false);
  setLED(PIN_LED3, false);

  // Cảm biến input
  pinMode(PIN_LDR, INPUT);    // ADC — không cần mode đặc biệt
  pinMode(PIN_PIR, INPUT);    // PIR digital output

  // Nút — INPUT_PULLUP: bình thường HIGH, nhấn = LOW
  pinMode(PIN_NUT1, INPUT_PULLUP);
  pinMode(PIN_NUT2, INPUT_PULLUP);
  pinMode(PIN_NUT3, INPUT_PULLUP);

  // WiFi + Blynk
  Blynk.begin(BLYNK_AUTH_TOKEN, WIFI_SSID, WIFI_PASS);

  // KÍCH HOẠT LẤY GIỜ INTERNET NGAY SAU KHI CÓ WIFI
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  // Timer
  timer.setInterval(2000L, capNhatLED2);   // Đọc LDR mỗi 2s
  timer.setInterval(200L,  capNhatLED3);   // Đọc PIR mỗi 200ms (cần nhanh hơn)
  timer.setInterval(50L,   kiemTraNut);    // Kiểm tra nút mỗi 50ms

  Serial.println("[Setup] Hoan tat. San sang nhan lenh.");
  guiTelegram(
    "🏠 Smart Lighting v2 khoi dong!\n"
    "LED1: Giong noi AI\n"
    "LED2: Cam bien LDR\n"
    "LED3: Cam bien PIR"
  );
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  Blynk.run();
  timer.run();
}