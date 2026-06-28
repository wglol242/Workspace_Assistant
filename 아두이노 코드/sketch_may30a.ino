#include <SPI.h>
#include <WiFiNINA.h>
#include <Arduino_LSM6DS3.h>
#include <vector>
#include <OneButton.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <U8g2_for_Adafruit_GFX.h>

const char* WIFI_SSID = "won";
const char* WIFI_PASSWORD = "19981009";

#define BUTTON_PIN 2
#define TFT_SCK   13
#define TFT_MOSI  11
#define TFT_DC    9
#define TFT_RST   8
#define TFT_CS   -1

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;
WiFiServer server(80);
OneButton btn(BUTTON_PIN, true);

String display_mode = "clock";
bool is_focus_mode_on = false;
unsigned long focus_start_time = 0;

String music_title = "Not Connected";
String music_artist = "";
int music_submenu = 0;

unsigned long last_gyro_check = 0;
const int GYRO_CHECK_INTERVAL = 100;

bool needs_display_update = true;
String display_update_reason = "boot";

unsigned long last_clock_refresh = 0;
String last_clock_text = "";
String last_clock_wifi_text = "";

struct Task {
  String id;
  String title;
};

struct MailItem {
  String subject;
  String fromName;
};

struct CalendarItem {
  String timeText;
  String subject;
};

std::vector<Task> tasks;
int current_task_idx = 0;
MailItem latestMail = {"메일 없음", ""};
std::vector<CalendarItem> todayEvents;

unsigned long last_task_advance = 0;
const unsigned long TASK_ADVANCE_INTERVAL = 8000UL;

void requestDisplayUpdate(const char* reason);
void clearScreenWhite();
void drawCenteredText(const String& text, int y, const uint8_t* font);
String ellipsizeText(const String& text, int maxWidth, const uint8_t* font);
void drawTopBar(const String& title, const String& rightText = "");
void drawCard(int x, int y, int w, int h);
void updateDisplay();
void updateClockOnly();
void showBootMessage(const char* line1, const char* line2);
void connect_wifi();

unsigned char h2int(char c);
String urlDecode(String str);
String getQueryParam(const String& request, const String& key);

void handleLongPressComplete();
void handleShortClick();
void check_sensor_and_control();
void advanceTodoTaskIfNeeded();

unsigned long getKstEpoch();
String formatTimeHHMM(unsigned long epoch);

void requestDisplayUpdate(const char* reason) {
  display_update_reason = String(reason);
  needs_display_update = true;
  Serial.print("[LCD_TRIGGER] reason=");
  Serial.print(reason);
  Serial.print(", mode=");
  Serial.println(display_mode);
}

void clearScreenWhite() {
  tft.fillScreen(ST77XX_WHITE);
  u8g2Fonts.setForegroundColor(ST77XX_BLACK);
  u8g2Fonts.setBackgroundColor(ST77XX_WHITE);
}

void drawCenteredText(const String& text, int y, const uint8_t* font) {
  u8g2Fonts.setFont(font);
  int w = u8g2Fonts.getUTF8Width(text.c_str());
  int x = (tft.width() - w) / 2;
  if (x < 0) x = 0;
  u8g2Fonts.setCursor(x, y);
  u8g2Fonts.print(text);
}

String ellipsizeText(const String& text, int maxWidth, const uint8_t* font) {
  u8g2Fonts.setFont(font);
  if (u8g2Fonts.getUTF8Width(text.c_str()) <= maxWidth) return text;

  String out = text;
  while (out.length() > 0) {
    String candidate = out + "...";
    if (u8g2Fonts.getUTF8Width(candidate.c_str()) <= maxWidth) return candidate;
    out.remove(out.length() - 1);
  }
  return "...";
}

void drawTopBar(const String& title, const String& rightText) {
  tft.fillRoundRect(6, 6, tft.width() - 12, 28, 6, ST77XX_BLACK);
  u8g2Fonts.setForegroundColor(ST77XX_WHITE);
  u8g2Fonts.setBackgroundColor(ST77XX_BLACK);

  u8g2Fonts.setFont(u8g2_font_7x14_tf);
  u8g2Fonts.setCursor(14, 24);
  u8g2Fonts.print(title);

  if (rightText.length() > 0) {
    int rw = u8g2Fonts.getUTF8Width(rightText.c_str());
    u8g2Fonts.setCursor(tft.width() - rw - 14, 24);
    u8g2Fonts.print(rightText);
  }

  u8g2Fonts.setForegroundColor(ST77XX_BLACK);
  u8g2Fonts.setBackgroundColor(ST77XX_WHITE);
}

void drawCard(int x, int y, int w, int h) {
  tft.drawRoundRect(x, y, w, h, 8, ST77XX_BLACK);
}

unsigned long getKstEpoch() {
  if (WiFi.status() != WL_CONNECTED) return 0;

  unsigned long epoch = WiFi.getTime();
  if (epoch == 0) return 0;

  return epoch + (9UL * 3600UL);
}

String formatTimeHHMM(unsigned long epoch) {
  if (epoch == 0) return "--:--";

  unsigned long secondsOfDay = epoch % 86400UL;
  int hour = secondsOfDay / 3600UL;
  int minute = (secondsOfDay % 3600UL) / 60UL;

  char buf[6];
  sprintf(buf, "%02d:%02d", hour, minute);
  return String(buf);
}

void updateClockOnly() {
  if (display_mode != "clock" || is_focus_mode_on) return;

  unsigned long kstEpoch = getKstEpoch();
  String timeText = formatTimeHHMM(kstEpoch);
  String wifiText = (WiFi.status() == WL_CONNECTED) ? "WiFi OK" : "WiFi OFF";

  if (timeText == last_clock_text && wifiText == last_clock_wifi_text) return;

  tft.setRotation(2);
  int w = tft.width();

  tft.fillRect(28, 100, w - 56, 62, ST77XX_WHITE);
  drawCenteredText(timeText, 145, u8g2_font_logisoso42_tf);

  tft.fillRect(w - 90, 208, 76, 16, ST77XX_WHITE);
  u8g2Fonts.setFont(u8g2_font_7x14_tf);
  u8g2Fonts.setCursor(w - 78, 220);
  u8g2Fonts.print(wifiText);

  last_clock_text = timeText;
  last_clock_wifi_text = wifiText;
}

void updateDisplay() {
  bool full_redraw = (display_update_reason == "boot" ||
                      display_update_reason == "imu_mode_changed" ||
                      display_update_reason == "focus_on" ||
                      display_update_reason == "focus_off");

  if (full_redraw) {
    clearScreenWhite();
  }

  if (display_mode == "clock") {
    tft.setRotation(2);
    int w = tft.width();

    if (is_focus_mode_on) {
      unsigned long elapsed = millis() - focus_start_time;
      int minutes = elapsed / 60000;
      String minStr = String(minutes);

      if (full_redraw) {
        drawTopBar("FOCUS MODE");
        drawCard(14, 46, w - 28, 150);
        drawCenteredText("집중 시간", 76, u8g2_font_unifont_t_korean2);
        drawCenteredText("MIN", 172, u8g2_font_7x14_tf);

        u8g2Fonts.setFont(u8g2_font_7x14_tf);
        u8g2Fonts.setCursor(20, 220);
        u8g2Fonts.print("Stay focused.");
      }

      tft.fillRect(20, 90, w - 40, 60, ST77XX_WHITE);
      drawCenteredText(minStr, 138, u8g2_font_logisoso50_tf);

    } else {
      unsigned long kstEpoch = getKstEpoch();
      String timeText = formatTimeHHMM(kstEpoch);
      String wifiText = (WiFi.status() == WL_CONNECTED) ? "WiFi OK" : "WiFi OFF";

      if (full_redraw) {
        drawTopBar("CLOCK");
        drawCard(14, 46, w - 28, 150);
        drawCenteredText("현재 시간", 76, u8g2_font_unifont_t_korean2);

        u8g2Fonts.setFont(u8g2_font_7x14_tf);
        u8g2Fonts.setCursor(20, 220);
        u8g2Fonts.print("KST");
      }

      tft.fillRect(28, 100, w - 56, 62, ST77XX_WHITE);
      drawCenteredText(timeText, 145, u8g2_font_logisoso42_tf);

      tft.fillRect(w - 90, 208, 76, 16, ST77XX_WHITE);
      u8g2Fonts.setFont(u8g2_font_7x14_tf);
      u8g2Fonts.setCursor(w - 78, 220);
      u8g2Fonts.print(wifiText);

      last_clock_text = timeText;
      last_clock_wifi_text = wifiText;
    }
  }
  else if (display_mode == "todo") {
    tft.setRotation(1);
    int w = tft.width();
    int h = tft.height();

    String rightInfo = String(current_task_idx + 1) + "/" + String(tasks.size());
    if (tasks.empty()) rightInfo = "0/0";

    drawTopBar("TODO", rightInfo);

    if (full_redraw) {
      drawCard(8, 42, w - 16, h - 54);
    }
    
    tft.fillRoundRect(10, 44, w - 20, h - 58, 6, ST77XX_WHITE);

    if (!tasks.empty()) {
      String title = tasks[current_task_idx].title;
      String line1 = ellipsizeText(title, w - 32, u8g2_font_unifont_t_korean2);

      u8g2Fonts.setFont(u8g2_font_7x14_tf);
      u8g2Fonts.setCursor(18, 66);
      u8g2Fonts.print("Current Task");

      tft.drawLine(18, 74, w - 18, 74, ST77XX_BLACK);

      u8g2Fonts.setFont(u8g2_font_unifont_t_korean2);
      u8g2Fonts.setCursor(18, 108);
      u8g2Fonts.print(line1);

      u8g2Fonts.setFont(u8g2_font_7x14_tf);
      u8g2Fonts.setCursor(18, 145);
      u8g2Fonts.print("Long press: complete");
    } else {
      drawCenteredText("할 일 없음", 105, u8g2_font_unifont_t_korean2);
      drawCenteredText("오늘은 좀 쉬어도 됨", 135, u8g2_font_7x14_tf);
    }
  }
  else if (display_mode == "music") {
    tft.setRotation(3);
    int w = tft.width();
    int h = tft.height();

    if (full_redraw) {
      drawCard(8, 42, w - 16, h - 54);
    }
    
    tft.fillRoundRect(10, 44, w - 20, h - 58, 6, ST77XX_WHITE);

    if (music_submenu == 0) {
      drawTopBar("NOW PLAYING", "1/3");

      String title = ellipsizeText(music_title, w - 32, u8g2_font_unifont_t_korean2);
      String artist = ellipsizeText(music_artist, w - 32, u8g2_font_unifont_t_korean2);

      u8g2Fonts.setFont(u8g2_font_7x14_tf);
      u8g2Fonts.setCursor(18, 66);
      u8g2Fonts.print("Title");

      u8g2Fonts.setFont(u8g2_font_unifont_t_korean2);
      u8g2Fonts.setCursor(18, 96);
      u8g2Fonts.print(title);

      tft.drawLine(18, 112, w - 18, 112, ST77XX_BLACK);

      u8g2Fonts.setFont(u8g2_font_7x14_tf);
      u8g2Fonts.setCursor(18, 138);
      u8g2Fonts.print("Artist");

      u8g2Fonts.setFont(u8g2_font_unifont_t_korean2);
      u8g2Fonts.setCursor(18, 168);
      u8g2Fonts.print(artist);
    }
    else if (music_submenu == 1) {
      drawTopBar("LATEST MAIL", "2/3");

      String subject = ellipsizeText(latestMail.subject, w - 32, u8g2_font_unifont_t_korean2);
      String from = ellipsizeText(latestMail.fromName, w - 32, u8g2_font_unifont_t_korean2);

      u8g2Fonts.setFont(u8g2_font_7x14_tf);
      u8g2Fonts.setCursor(18, 66);
      u8g2Fonts.print("Subject");

      u8g2Fonts.setFont(u8g2_font_unifont_t_korean2);
      u8g2Fonts.setCursor(18, 96);
      u8g2Fonts.print(subject);

      tft.drawLine(18, 112, w - 18, 112, ST77XX_BLACK);

      u8g2Fonts.setFont(u8g2_font_7x14_tf);
      u8g2Fonts.setCursor(18, 138);
      u8g2Fonts.print("From");

      u8g2Fonts.setFont(u8g2_font_unifont_t_korean2);
      u8g2Fonts.setCursor(18, 168);
      u8g2Fonts.print(from);
    }
    else if (music_submenu == 2) {
      drawTopBar("TODAY", "3/3");

      if (!todayEvents.empty()) {
        String timeText = ellipsizeText(todayEvents[0].timeText, w - 32, u8g2_font_7x14_tf);
        String subject = ellipsizeText(todayEvents[0].subject, w - 32, u8g2_font_unifont_t_korean2);

        u8g2Fonts.setFont(u8g2_font_7x14_tf);
        u8g2Fonts.setCursor(18, 66);
        u8g2Fonts.print("Time");

        u8g2Fonts.setCursor(18, 92);
        u8g2Fonts.print(timeText);

        tft.drawLine(18, 108, w - 18, 108, ST77XX_BLACK);

        u8g2Fonts.setFont(u8g2_font_7x14_tf);
        u8g2Fonts.setCursor(18, 132);
        u8g2Fonts.print("Subject");

        u8g2Fonts.setFont(u8g2_font_unifont_t_korean2);
        u8g2Fonts.setCursor(18, 164);
        u8g2Fonts.print(subject);
      } else {
        drawCenteredText("일정 없음", 115, u8g2_font_unifont_t_korean2);
      }
    }
  }

  needs_display_update = false;
}

void showBootMessage(const char* line1, const char* line2) {
  tft.setRotation(2);
  clearScreenWhite();

  drawTopBar("BOOT");
  drawCard(10, 60, tft.width() - 20, 90);

  drawCenteredText(String(line1), 100, u8g2_font_unifont_t_korean2);
  drawCenteredText(String(line2), 128, u8g2_font_7x14_tf);
}

void connect_wifi() {
  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println("WiFi module not found!");
    while (true);
  }

  int status = WL_IDLE_STATUS;
  int retry = 0;

  while (status != WL_CONNECTED && retry < 20) {
    Serial.print("Connecting to WiFi: ");
    Serial.println(WIFI_SSID);

    status = WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    delay(5000);
    retry++;
  }

  if (status == WL_CONNECTED) {
    Serial.println("Connected. IP:");
    Serial.println(WiFi.localIP());

    unsigned long epoch = 0;
    int timeRetry = 0;
    while (epoch == 0 && timeRetry < 10) {
      epoch = WiFi.getTime();
      if (epoch == 0) {
        Serial.println("Waiting for network time...");
        delay(1000);
      }
      timeRetry++;
    }

    Serial.print("Epoch: ");
    Serial.println(epoch);

    showBootMessage("WiFi 연결됨!", WiFi.localIP().toString().c_str());
    requestDisplayUpdate("wifi_connected");
  } else {
    Serial.println("WiFi connection failed");
    showBootMessage("WiFi 실패", "SSID/비번 확인");
  }
}

unsigned char h2int(char c) {
  if (c >= '0' && c <= '9') return ((unsigned char)c - '0');
  if (c >= 'a' && c <= 'f') return ((unsigned char)c - 'a' + 10);
  if (c >= 'A' && c <= 'F') return ((unsigned char)c - 'A' + 10);
  return 0;
}

String urlDecode(String str) {
  String decoded = "";
  int len = str.length();
  for (int i = 0; i < len; i++) {
    char c = str.charAt(i);
    if (c == '+') decoded += ' ';
    else if (c == '%' && i + 2 < len) {
      unsigned char code0 = str.charAt(++i);
      unsigned char code1 = str.charAt(++i);
      decoded += (char)((h2int(code0) << 4) | h2int(code1));
    } else {
      decoded += c;
    }
  }
  return decoded;
}

String getQueryParam(const String& request, const String& key) {
  String pattern = key + "=";
  int start = request.indexOf(pattern);
  if (start == -1) return "";

  start += pattern.length();
  int end = request.indexOf("&", start);
  if (end == -1) end = request.indexOf(" ", start);
  if (end == -1) end = request.length();

  return urlDecode(request.substring(start, end));
}

void handleLongPressComplete() {
  if (display_mode == "todo" && tasks.size() > 0) {
    tft.setRotation(1);
    
    tft.fillRoundRect(10, 44, tft.width() - 20, tft.height() - 58, 6, ST77XX_WHITE);
    drawTopBar("TODO");
    drawCard(20, 70, tft.width() - 40, 80);
    
    drawCenteredText("완료!!", 118, u8g2_font_unifont_t_korean2);

    Serial.print("[TODO_COMPLETE_REQUEST] ");
    Serial.println(tasks[current_task_idx].title);

    const char* pc_ip = "192.168.0.209";  
    int pc_port = 8080;

    WiFiClient client;
    
    if (client.connect(pc_ip, pc_port)) {
        client.println("GET /completeTodo HTTP/1.1");
        client.print("Host: ");
        client.println(pc_ip);
        client.println("Connection: close");
        client.println();
        client.stop();

        delay(500); 

    } else {
        tft.fillRect(22, 90, tft.width() - 44, 40, ST77XX_WHITE);
        drawCenteredText("PC 연결 실패", 118, u8g2_font_unifont_t_korean2);
        Serial.println("PC 연결 실패 - IP를 확인하거나 C# 브릿지가 관리자 권한인지 확인하세요");
        
        delay(1500);
    }

    if (tasks.size() > 0) {
      tasks.erase(tasks.begin() + current_task_idx);
      if (current_task_idx >= (int)tasks.size()) current_task_idx = 0;
      requestDisplayUpdate("todo_removed_local");
    }
  }
}

void handleShortClick() {
  if (display_mode == "music") {
    music_submenu = (music_submenu + 1) % 3;
    requestDisplayUpdate("music_submenu_changed");
  }
}

void check_sensor_and_control() {
  float ax, ay, az;

  if (IMU.accelerationAvailable()) {
    IMU.readAcceleration(ax, ay, az);

    float threshold_mode = 0.8f;
    String new_mode = display_mode;

    if (ay > threshold_mode) new_mode = "music";
    else if (ay < -threshold_mode) new_mode = "todo";
    else new_mode = "clock";

    if (new_mode != display_mode) {
      display_mode = new_mode;

      if (display_mode == "todo") last_task_advance = millis();
      if (display_mode == "music") music_submenu = 0;
      if (display_mode == "clock") {
        last_clock_text = "";
        last_clock_wifi_text = "";
      }

      requestDisplayUpdate("imu_mode_changed");
    }
  }
}

void advanceTodoTaskIfNeeded() {
  if (btn.isLongPressed()) return;
  if (display_mode != "todo") return;
  if (tasks.size() <= 1) return;

  unsigned long now = millis();
  if (now - last_task_advance >= TASK_ADVANCE_INTERVAL) {
    current_task_idx = (current_task_idx + 1) % tasks.size();
    last_task_advance = now;
    requestDisplayUpdate("todo_auto_advance");
  }
}

void setup() {
  Serial.begin(115200);

  if (!IMU.begin()) {
    Serial.println("Failed to initialize IMU!");
  } else {
    Serial.println("IMU OK");
  }

  SPI.begin();

  tft.init(240, 240, SPI_MODE3);
  tft.setRotation(2);
  tft.fillScreen(ST77XX_WHITE);

  u8g2Fonts.begin(tft);
  u8g2Fonts.setFont(u8g2_font_unifont_t_korean2);
  u8g2Fonts.setFontMode(1);
  u8g2Fonts.setFontDirection(0);

  showBootMessage("기기 부팅중...", "WiFi 연결 대기");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  btn.attachLongPressStart(handleLongPressComplete);
  btn.attachClick(handleShortClick);

  connect_wifi();
  server.begin();

  Serial.print("HTTP server started: http://");
  Serial.println(WiFi.localIP());

  Serial.println("Setup complete.");
}

void loop() {
  unsigned long current_time_ms = millis();
  btn.tick();

  if (current_time_ms - last_gyro_check > (unsigned long)GYRO_CHECK_INTERVAL) {
    check_sensor_and_control();
    last_gyro_check = current_time_ms;
  }

  WiFiClient client = server.available();
  if (client) {
    String request = "";
    unsigned long timeout = millis() + 100;

    while (client.connected() && millis() < timeout) {
      while (client.available()) {
        request += (char)client.read();
      }
    }

    Serial.println("---- HTTP REQUEST ----");
    Serial.println(request);

    if (request.indexOf("GET /") != -1) {
      if (request.indexOf("/focus") != -1) {
        String status = getQueryParam(request, "status");
        if (status == "on") {
          is_focus_mode_on = true;
          focus_start_time = millis();
          requestDisplayUpdate("focus_on");
        } else if (status == "off") {
          is_focus_mode_on = false;
          requestDisplayUpdate("focus_off");
        }
      }

      if (request.indexOf("/music") != -1) {
        String title = getQueryParam(request, "title");
        String artist = getQueryParam(request, "artist");

        if (title.length() > 0) music_title = title;
        music_artist = artist;

        if (display_mode == "music" && music_submenu == 0) {
          requestDisplayUpdate("music_metadata_changed");
        }
      }

      if (request.indexOf("/mail") != -1) {
        latestMail.subject = getQueryParam(request, "subject");
        latestMail.fromName = getQueryParam(request, "from");
        requestDisplayUpdate("mail_updated");
      }

      if (request.indexOf("/todo") != -1) {
        String title = getQueryParam(request, "title");
        tasks.clear();
        if (title.length() > 0) {
          tasks.push_back({"local-0", title});
        }
        current_task_idx = 0;
        last_task_advance = millis();
        requestDisplayUpdate("todo_updated");
      }
      if (request.indexOf("/calendar") != -1) {
        String timeText = getQueryParam(request, "time");
        String subject = getQueryParam(request, "subject");

        int tIndex = timeText.indexOf('T');
        if (tIndex != -1) {
          timeText = timeText.substring(0, tIndex); 
        }

        todayEvents.clear();
        if (subject.length() > 0 || timeText.length() > 0) {
          todayEvents.push_back({timeText, subject});
        }
        requestDisplayUpdate("calendar_updated");
      }

      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: text/plain; charset=utf-8");
      client.println("Connection: close");
      client.println();
      client.println("OK");
    }

    client.stop();
  }

  advanceTodoTaskIfNeeded();

  if (display_mode == "clock" && !is_focus_mode_on) {
    unsigned long now = millis();
    if (now - last_clock_refresh >= 1000UL) {
      last_clock_refresh = now;
      updateClockOnly();
    }
  }

  if (needs_display_update) {
    updateDisplay();
  }
}