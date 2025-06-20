#include <LilyGo_AMOLED.h>
#include <LV_Helper.h>
#include <lvgl.h>
#include <time.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

LilyGo_Class amoled;

static lv_obj_t *label_temp;

//color wheel tabs different light, select different lights en colorpicking, graph temp/presence sensor

const char *ssid = "arda";
const char *password = "voorschool123";
String gekozen_stad = "Enschede";

static lv_obj_t *label_slider;
static lv_timer_t *fade_timer;
static lv_obj_t *label_time_global = NULL;  // Globale referentie klok

void fade_out_label(lv_timer_t *timer);
void connectWiFi() {
  Serial.print("Verbinden met WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" Verbonden");
}

void initTime() {
  configTime(3600, 3600, "pool.ntp.org", "time.nist.gov");

  Serial.print("Wachten op tijd synchronisatie");
  time_t now = time(nullptr);
  while (now < 8 * 3600 * 2) {  // wacht tot tijd gesynchroniseerd is
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println(" Tijd gesynchroniseerd!");
}

void update_time_display() {
  if (!label_time_global) return;

  time_t now = time(nullptr);
  struct tm *tm_info = localtime(&now);

  char buffer[6];
  strftime(buffer, sizeof(buffer), "%H:%M", tm_info);
  lv_label_set_text(label_time_global, buffer);
}
float fetch_temperature_from_city(const String &city) {
  String url = "https://wttr.in/" + city + "?format=j1";

  HTTPClient http;
  http.begin(url);
  int httpCode = http.GET();

  if (httpCode != 200) {
    Serial.println("Fout bij ophalen temperatuur");
    http.end();
    return -1000.0;
  }

  String payload = http.getString();
  StaticJsonDocument<4096> doc;
  DeserializationError err = deserializeJson(doc, payload);
  Serial.println(payload);


  if (err) {
    Serial.println("JSON parse fout");
    http.end();
    return -1000.0;
  }

  const char *tempC = doc["current_condition"][0]["temp_C"];
  if (tempC == nullptr) {
    Serial.println("Geen temperatuurdata");
    http.end();
    return -1000.0;
  }

  float temp = atof(tempC);
  http.end();
  return temp;
}
void create_temperature_label() {
  label_temp = lv_label_create(lv_scr_act());
  lv_obj_set_style_text_color(label_temp, lv_color_white(), 0);
  lv_obj_set_style_text_font(label_temp, &lv_font_montserrat_20, 0);
  lv_obj_align(label_temp, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_label_set_text(label_temp, "Temp: -- °C");
}

void update_temperature_display() {
  float temp = fetch_temperature_from_city(gekozen_stad);
  if (temp > -100) {
    const char *symbool;
    if (temp >= 20) {
      symbool = "\u2600";
    } else {
      symbool = "\u2601";
    }
    lv_label_set_text_fmt(label_temp, "%s: %.1f °C", gekozen_stad.c_str(), temp);
  } else {
    lv_label_set_text(label_temp, "Temp: fout");
  }
}

void create_smart_home_ui() {
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x121212), 0);

  lv_obj_t *title = lv_label_create(lv_scr_act());
  lv_label_set_text(title, LV_SYMBOL_HOME " Smart Home");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(title, lv_palette_lighten(LV_PALETTE_BLUE_GREY, 3), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

  lv_obj_t *light_btn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(light_btn, 180, 50);
  lv_obj_align(light_btn, LV_ALIGN_CENTER, 0, -40);
  lv_obj_add_flag(light_btn, LV_OBJ_FLAG_CHECKABLE);
  lv_obj_set_style_radius(light_btn, 12, 0);
  lv_obj_set_style_shadow_width(light_btn, 15, 0);
  lv_obj_set_style_shadow_opa(light_btn, LV_OPA_30, 0);

  lv_obj_set_style_bg_color(light_btn, lv_palette_main(LV_PALETTE_BLUE), 0);

  lv_obj_t *light_label = lv_label_create(light_btn);
  lv_label_set_text(light_label, "Licht: START");
  lv_obj_center(light_label);

  static bool first_toggle = false;

  lv_obj_add_event_cb(
    light_btn, [](lv_event_t *e) {
      lv_obj_t *btn = lv_event_get_target(e);
      lv_obj_t *label = lv_obj_get_child(btn, 0);

           if (lv_obj_has_state(btn, LV_STATE_CHECKED)) {
        // Aan
        lv_label_set_text(label, LV_SYMBOL_POWER " Licht: UIT");
        lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_RED), 0);
      } else {
        // Uit
        lv_label_set_text(label, LV_SYMBOL_POWER " Licht: AAN");
        lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_GREEN), 0);
      }
    },
    LV_EVENT_CLICKED, NULL);  // LET OP: Event is LV_EVENT_CLICKED

  lv_obj_t *slider = lv_slider_create(lv_scr_act());
  lv_obj_set_width(slider, 220);
  lv_obj_align(slider, LV_ALIGN_CENTER, 0, 40);
  lv_slider_set_range(slider, 0, 100);
  lv_slider_set_value(slider, 50, LV_ANIM_OFF);

  static lv_style_t style_slider, style_knob;
  lv_style_init(&style_slider);
  lv_style_set_bg_color(&style_slider, lv_color_hex(0x1E1E1E));
  lv_style_set_radius(&style_slider, 10);
  lv_obj_add_style(slider, &style_slider, LV_PART_MAIN);

  lv_style_init(&style_knob);
  lv_style_set_bg_color(&style_knob, lv_palette_main(LV_PALETTE_CYAN));
  lv_style_set_radius(&style_knob, LV_RADIUS_CIRCLE);
  lv_style_set_size(&style_knob, 20);
  lv_obj_add_style(slider, &style_knob, LV_PART_KNOB);

  label_slider = lv_label_create(lv_scr_act());
  lv_label_set_text(label_slider, "Helderheid: 50%");
  lv_obj_set_style_text_color(label_slider, lv_palette_lighten(LV_PALETTE_CYAN, 3), 0);
  lv_obj_set_style_text_font(label_slider, &lv_font_montserrat_16, 0);
  lv_obj_set_style_bg_color(label_slider, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(label_slider, LV_OPA_70, 0);
  lv_obj_set_style_pad_all(label_slider, 6, 0);
  lv_obj_set_style_radius(label_slider, 8, 0);
  lv_obj_set_style_opa(label_slider, LV_OPA_TRANSP, 0);
  lv_obj_align_to(label_slider, slider, LV_ALIGN_OUT_TOP_MID, 0, -10);

  lv_obj_add_event_cb(
    slider, [](lv_event_t *e) {
      lv_obj_t *slider = lv_event_get_target(e);
      int32_t val = lv_slider_get_value(slider);

      lv_label_set_text_fmt(label_slider, "Helderheid: %d%%", val);
      lv_obj_align_to(label_slider, slider, LV_ALIGN_OUT_TOP_MID, 0, -10);

      lv_anim_t a;
      lv_anim_init(&a);
      lv_anim_set_var(&a, label_slider);
      lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
      lv_anim_set_values(&a, lv_obj_get_style_opa(label_slider, 0), LV_OPA_COVER);
      lv_anim_set_time(&a, 300);
      lv_anim_start(&a);

      if (fade_timer) lv_timer_reset(fade_timer);
    },
    LV_EVENT_VALUE_CHANGED, NULL);

  fade_timer = lv_timer_create(fade_out_label, 2000, label_slider);
}

void fade_out_label(lv_timer_t *timer) {
  lv_obj_t *label = (lv_obj_t *)timer->user_data;

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, label);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
  lv_anim_set_values(&a, lv_obj_get_style_opa(label, 0), LV_OPA_TRANSP);
  lv_anim_set_time(&a, 1000);
  lv_anim_start(&a);
}

void setup() {
  Serial.begin(115200);
  connectWiFi();  // WiFi verbinden
  initTime();
  bool rslt = amoled.begin();
  if (!rslt) {
    while (1) {
      Serial.println("Kan board niet detecteren, verhoog Core Debug Level.");
      delay(1000);
    }
  }

  beginLvglHelper(amoled);
  lv_obj_t *label_wifi = lv_label_create(lv_scr_act());
  lv_label_set_text(label_wifi, LV_SYMBOL_WIFI);  // Gebruik LVGL wifi symbool
  lv_obj_set_style_text_color(label_wifi, lv_color_white(), 0);
  lv_obj_align(label_wifi, LV_ALIGN_TOP_RIGHT, -10, 10);
  lv_obj_t *label_battery = lv_label_create(lv_scr_act());
  lv_label_set_text(label_battery, LV_SYMBOL_BATTERY_3);  // Je kunt ook _FULL, _2, _EMPTY gebruiken
  lv_obj_set_style_text_color(label_battery, lv_color_white(), 0);
  lv_obj_align_to(label_battery, label_wifi, LV_ALIGN_OUT_LEFT_MID, -10, 0);
  label_time_global = lv_label_create(lv_scr_act());
  lv_label_set_text(label_time_global, "13:08");
  lv_obj_set_style_text_color(label_time_global, lv_color_white(), 0);
  lv_obj_align_to(label_time_global, label_battery, LV_ALIGN_OUT_LEFT_MID, -10, 0);
  create_temperature_label();
  update_temperature_display();
  create_smart_home_ui();
  update_time_display();  // Initiale tijd tonen
}
void loop() {
  lv_task_handler();
  static unsigned long last_update = 0;
  if (millis() - last_update > 60000 || last_update == 0) {  // Elke minuut updaten
    update_time_display();
    last_update = millis();
  }
  static unsigned long last_temp_update = 0;
  if (millis() - last_temp_update > 600000 || last_temp_update == 0) {
    update_temperature_display();
    last_temp_update = millis();
  }


  delay(5);
}
