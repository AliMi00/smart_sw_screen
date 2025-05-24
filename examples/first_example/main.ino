#include <Arduino.h>
#include <LilyGo_AMOLED.h>
#include <LV_Helper.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <lvgl.h>
#include <AceButton.h>
#include <Adafruit_NeoPixel.h>
#include <time.h>

/**
 * Smart Light Controller for LilyGo T4 S3 AMOLED Display
 * --------------------------------------------------------
 * This project creates a smart light switch control panel
 * that connects to Home Assistant to control lights and
 * display sensor information.
 * 
 * SETUP:
 * 1. Update the WiFi credentials below
 * 2. Update the Home Assistant URL and token
 * 3. Set your light entity ID from Home Assistant
 * 
 * FEATURES:
 * - Light control (on/off, brightness)
 * - Temperature and humidity display
 * - Multiple screens with simple navigation
 * - Power saving with deep sleep mode
 * 
 * NAVIGATION:
 * - Press the physical button to cycle through screens
 * - Long press for sleep mode
 * - Home button also cycles through screens
 */

using namespace ace_button;

// WiFi credentials
#ifndef WIFI_SSID
#define WIFI_SSID             "TP-Link_D47E"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD         "75573768"
#endif

// Home Assistant settings
#define HOME_ASSISTANT_URL    "http://192.168.1.102:8123"
#define HOME_ASSISTANT_TOKEN  "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJhNDczMWE3MjMxMzQ0NzU3YjI5NThiZjM2MTE4YTQzMCIsImlhdCI6MTc0ODA4Nzc0NSwiZXhwIjoyMDYzNDQ3NzQ1fQ.Y0TU3w5c_niT5xtVVQAjYMNclr0jAvndL_m5Q74OD34"
#define LIGHT_ENTITY_ID       "light.lamp_2"

// Message IDs for LVGL
#define WIFI_MSG_ID             0x1001
#define HA_CONNECTION_MSG_ID    0x1002
#define LIGHT_STATUS_MSG_ID     0x1003
#define TEMPERATURE_MSG_ID      0x1004
#define HUMIDITY_MSG_ID         0x1005

// GUI states
enum GUI_STATE {
    STATE_MAIN,
    STATE_BRIGHTNESS,
    STATE_INFO,
    STATE_SETTINGS
};

// Global variables
LilyGo_Class amoled;
Adafruit_NeoPixel *pixels = NULL;
AceButton *button = NULL;
GUI_STATE currentState = STATE_MAIN;
volatile bool sleep_flag = false;
bool lightState = false;
int brightnessValue = 100;
double temperature = 21.5;
double humidity = 45.0;
String haVersion = "";
String timezone = "";
static TaskHandle_t vUpdateHAStateTaskHandler = NULL;
static TaskHandle_t vUpdateSensorTaskHandler = NULL;
static SemaphoreHandle_t xWiFiLock = NULL;
WiFiClient client; // Using non-secure client as default
HTTPClient http; // Using regular HTTP client

// Forward declarations
void setupGUI();
void mainScreenGUI();
void brightnessScreenGUI();
void infoScreenGUI();
void settingsScreenGUI();
void buttonHandlerTask(void *ptr);
void buttonHandleEvent(AceButton *button, uint8_t eventType, uint8_t buttonState);
void WiFiEvent(WiFiEvent_t event);
void updateHAStateTask(void *ptr);
void updateSensorTask(void *ptr);
void toggleLight();
void setBrightness(int brightness);
bool getHAState();
void switchToScreen(GUI_STATE state);
void enterSleepMode();
bool handleHTTPResponse(HTTPClient &http, int httpResponseCode, const char* operation);

// LVGL Objects
static lv_obj_t *main_screen;
static lv_obj_t *brightness_screen;
static lv_obj_t *info_screen;
static lv_obj_t *settings_screen;
static lv_obj_t *light_switch;
static lv_obj_t *brightness_slider;
static lv_obj_t *temperature_label;
static lv_obj_t *humidity_label;
static lv_obj_t *time_label;
static lv_obj_t *date_label;
static lv_obj_t *wifi_label;

void setup() {
    Serial.begin(115200);
    Serial.println("Smart Light Controller Starting...");

    // Initialize semaphore for WiFi tasks
    xWiFiLock = xSemaphoreCreateBinary();
    xSemaphoreGive(xWiFiLock);

    // Register WiFi event
    WiFi.onEvent(WiFiEvent);

    // Initialize the display
    bool rslt = amoled.begin();
    if (!rslt) {
        Serial.println("AMOLED initialization failed!");
        while (1) {
            delay(100);
        }
    }

    Serial.println("============================================");
    Serial.print("    Board Name: LilyGo AMOLED "); 
    Serial.println(amoled.getName());
    Serial.println("============================================");

    // Register lvgl helper
    beginLvglHelper(amoled);

    // Get board configuration
    const BoardsConfigure_t *boards = amoled.getBoardsConfigure();    // Setup buttons if available
    if (boards->buttonNum) {
        ButtonConfig *buttonConfig;
        button = new AceButton[boards->buttonNum];
        
        // Setup according to board variant
        // For T4 S3 the button is typically connected to GPIO0
        // Alternative solution:
        uint8_t pin = 0;
        uint8_t defaultState = HIGH;
        uint8_t id = 0;
        button[0].init(pin, defaultState, id);
        // Setup ButtonConfig with the event handler
        buttonConfig = ButtonConfig::getSystemButtonConfig();
        buttonConfig->setEventHandler(buttonHandleEvent);
        buttonConfig->setFeature(ButtonConfig::kFeatureClick);
        buttonConfig->setFeature(ButtonConfig::kFeatureDoubleClick);
        buttonConfig->setFeature(ButtonConfig::kFeatureLongPress);

        // Setup NeoPixels if available
        if (boards->pixelsPins != -1) {
            pixels = new Adafruit_NeoPixel(1, boards->pixelsPins, NEO_GRB + NEO_KHZ800);
            pixels->begin();
            pixels->setBrightness(50);
            pixels->clear();
            pixels->show();
        }
    }

    // Setup GUI
    setupGUI();    // Connect to WiFi
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("Connecting to WiFi (%s)...\n", WIFI_SSID);

    // Enable Watchdog
    enableLoopWDT();

    // Create button handler task
    if (boards->buttonNum && button) {
        xTaskCreate(buttonHandlerTask, "btn", 5 * 1024, NULL, 12, NULL);
    }

    // Set home button callback
    amoled.setHomeButtonCallback([](void *ptr) {
        Serial.println("Home key pressed!");
        static uint32_t checkMs = 0;
        if (millis() > checkMs) {
            // Cycle through screens when home button is pressed
            currentState = (GUI_STATE)((currentState + 1) % 4);
            switchToScreen(currentState);
            checkMs = millis() + 200;
        }
    }, NULL);
}

void loop() {
    if (sleep_flag) {
        return;
    }

    // Check WiFi connection status every 5 seconds
    static uint32_t last_check_connected = 0;
    if (last_check_connected < millis()) {
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("WiFi disconnected, attempting to reconnect...");
            WiFi.reconnect();
        }
        last_check_connected = millis() + 5000;
    }

    // Handle LVGL tasks
    lv_task_handler();
    delay(5);
}

void setupGUI() {
    // Create screens
    main_screen = lv_obj_create(NULL);
    brightness_screen = lv_obj_create(NULL);
    info_screen = lv_obj_create(NULL);
    settings_screen = lv_obj_create(NULL);

    // Initialize all screens
    mainScreenGUI();
    brightnessScreenGUI();
    infoScreenGUI();
    settingsScreenGUI();

    // Display main screen initially
    switchToScreen(STATE_MAIN);
}

void mainScreenGUI() {
    // Create a title label
    lv_obj_t *title = lv_label_create(main_screen);
    lv_label_set_text(title, "Smart Light Control");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);

    // Create a light switch
    light_switch = lv_switch_create(main_screen);
    lv_obj_align(light_switch, LV_ALIGN_CENTER, 0, -40);
    lv_obj_add_event_cb(light_switch, [](lv_event_t *e) {
        lightState = lv_obj_has_state(light_switch, LV_STATE_CHECKED);
        toggleLight();
    }, LV_EVENT_VALUE_CHANGED, NULL);
    
    // Label for the switch
    lv_obj_t *switch_label = lv_label_create(main_screen);
    lv_label_set_text(switch_label, "Light");
    lv_obj_align_to(switch_label, light_switch, LV_ALIGN_OUT_TOP_MID, 0, -10);

    // Create brightness shortcut button
    lv_obj_t *brightness_btn = lv_btn_create(main_screen);
    lv_obj_align(brightness_btn, LV_ALIGN_CENTER, 0, 20);
    lv_obj_add_event_cb(brightness_btn, [](lv_event_t *e) {
        switchToScreen(STATE_BRIGHTNESS);
    }, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *brightness_label = lv_label_create(brightness_btn);
    lv_label_set_text(brightness_label, "Brightness");
    lv_obj_center(brightness_label);

    // Create info shortcut button
    lv_obj_t *info_btn = lv_btn_create(main_screen);
    lv_obj_align(info_btn, LV_ALIGN_CENTER, 0, 70);
    lv_obj_add_event_cb(info_btn, [](lv_event_t *e) {
        switchToScreen(STATE_INFO);
    }, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *info_label = lv_label_create(info_btn);
    lv_label_set_text(info_label, "Info");
    lv_obj_center(info_label);

    // Create WiFi status indicator
    wifi_label = lv_label_create(main_screen);
    lv_label_set_text(wifi_label, "WiFi: Connecting...");
    lv_obj_align(wifi_label, LV_ALIGN_BOTTOM_LEFT, 10, -10);

    // Create time display
    time_label = lv_label_create(main_screen);
    lv_label_set_text(time_label, "00:00");
    lv_obj_align(time_label, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
}

void brightnessScreenGUI() {
    // Create a title label
    lv_obj_t *title = lv_label_create(brightness_screen);
    lv_label_set_text(title, "Brightness Control");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);    // Create a slider for brightness control
    brightness_slider = lv_slider_create(brightness_screen);
    lv_obj_set_width(brightness_slider, lv_pct(80));
    lv_obj_align(brightness_slider, LV_ALIGN_CENTER, 0, 0);
    lv_slider_set_range(brightness_slider, 0, 100);
    lv_slider_set_value(brightness_slider, brightnessValue, LV_ANIM_OFF);
    
    // Only update brightness on release to avoid too many API calls
    lv_obj_add_event_cb(brightness_slider, [](lv_event_t *e) {
        // Just update the brightness value
        brightnessValue = lv_slider_get_value(brightness_slider);
    }, LV_EVENT_VALUE_CHANGED, NULL);
    
    // Send API request only when slider is released
    lv_obj_add_event_cb(brightness_slider, [](lv_event_t *e) {
        brightnessValue = lv_slider_get_value(brightness_slider);
        Serial.printf("Slider released, setting brightness to: %d%%\n", brightnessValue);
        setBrightness(brightnessValue);
    }, LV_EVENT_RELEASED, NULL);

    // Create label for brightness percentage
    lv_obj_t *brightness_value_label = lv_label_create(brightness_screen);
    lv_label_set_text_fmt(brightness_value_label, "%d%%", brightnessValue);
    lv_obj_align_to(brightness_value_label, brightness_slider, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
    
    // Update the label when the slider value changes
    lv_obj_add_event_cb(brightness_slider, [](lv_event_t *e) {
        lv_obj_t *label = (lv_obj_t*)lv_event_get_user_data(e);
        lv_label_set_text_fmt(label, "%d%%", lv_slider_get_value(brightness_slider));
    }, LV_EVENT_VALUE_CHANGED, brightness_value_label);

    // Create back button
    lv_obj_t *back_btn = lv_btn_create(brightness_screen);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_add_event_cb(back_btn, [](lv_event_t *e) {
        switchToScreen(STATE_MAIN);
    }, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);
}

void infoScreenGUI() {
    // Create a title label
    lv_obj_t *title = lv_label_create(info_screen);
    lv_label_set_text(title, "System Information");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);

    // Create temperature display
    lv_obj_t *temp_title = lv_label_create(info_screen);
    lv_label_set_text(temp_title, "Temperature:");
    lv_obj_align(temp_title, LV_ALIGN_TOP_LEFT, 20, 50);
    
    temperature_label = lv_label_create(info_screen);
    lv_label_set_text_fmt(temperature_label, "%.1f °C", temperature);
    lv_obj_align_to(temperature_label, temp_title, LV_ALIGN_OUT_RIGHT_MID, 10, 0);

    // Create humidity display
    lv_obj_t *humid_title = lv_label_create(info_screen);
    lv_label_set_text(humid_title, "Humidity:");
    lv_obj_align(humid_title, LV_ALIGN_TOP_LEFT, 20, 80);
    
    humidity_label = lv_label_create(info_screen);
    lv_label_set_text_fmt(humidity_label, "%.1f %%", humidity);
    lv_obj_align_to(humidity_label, humid_title, LV_ALIGN_OUT_RIGHT_MID, 10, 0);    // Create Home Assistant info
    lv_obj_t *ha_title = lv_label_create(info_screen);
    lv_label_set_text(ha_title, "HA Version:");
    lv_obj_align(ha_title, LV_ALIGN_TOP_LEFT, 20, 110);
    
    lv_obj_t *ha_version_label = lv_label_create(info_screen);
    lv_label_set_text(ha_version_label, haVersion.isEmpty() ? "Connecting..." : haVersion.c_str());
    lv_obj_align_to(ha_version_label, ha_title, LV_ALIGN_OUT_RIGHT_MID, 10, 0);

    // Create date display
    date_label = lv_label_create(info_screen);
    lv_label_set_text(date_label, "Loading date...");
    lv_obj_align(date_label, LV_ALIGN_TOP_LEFT, 20, 140);

    // Create back button
    lv_obj_t *back_btn = lv_btn_create(info_screen);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_add_event_cb(back_btn, [](lv_event_t *e) {
        switchToScreen(STATE_MAIN);
    }, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);
}

void settingsScreenGUI() {
    // Create a title label
    lv_obj_t *title = lv_label_create(settings_screen);
    lv_label_set_text(title, "Settings");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);

    // Create WiFi settings
    lv_obj_t *wifi_title = lv_label_create(settings_screen);
    lv_label_set_text(wifi_title, "WiFi:");
    lv_obj_align(wifi_title, LV_ALIGN_TOP_LEFT, 20, 50);
    
    lv_obj_t *wifi_status = lv_label_create(settings_screen);
    lv_label_set_text(wifi_status, WiFi.SSID().c_str());
    lv_obj_align_to(wifi_status, wifi_title, LV_ALIGN_OUT_RIGHT_MID, 10, 0);    // Create timezone display
    lv_obj_t *tz_title = lv_label_create(settings_screen);
    lv_label_set_text(tz_title, "Timezone:");
    lv_obj_align(tz_title, LV_ALIGN_TOP_LEFT, 20, 80);
    
    lv_obj_t *tz_label = lv_label_create(settings_screen);
    lv_label_set_text(tz_label, timezone.isEmpty() ? "Not set" : timezone.c_str());
    lv_obj_align_to(tz_label, tz_title, LV_ALIGN_OUT_RIGHT_MID, 10, 0);

    // Create sleep button
    lv_obj_t *sleep_btn = lv_btn_create(settings_screen);
    lv_obj_align(sleep_btn, LV_ALIGN_CENTER, 0, 20);
    lv_obj_add_event_cb(sleep_btn, [](lv_event_t *e) {
        sleep_flag = true;
        // Enter deep sleep mode
        enterSleepMode();
    }, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *sleep_label = lv_label_create(sleep_btn);
    lv_label_set_text(sleep_label, "Sleep");
    lv_obj_center(sleep_label);

    // Create back button
    lv_obj_t *back_btn = lv_btn_create(settings_screen);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_add_event_cb(back_btn, [](lv_event_t *e) {
        switchToScreen(STATE_MAIN);
    }, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);
}

void switchToScreen(GUI_STATE state) {
    lv_obj_t *targetScreen;
    currentState = state;
    
    switch (state) {
        case STATE_MAIN:
            targetScreen = main_screen;
            break;
        case STATE_BRIGHTNESS:
            targetScreen = brightness_screen;
            break;
        case STATE_INFO:
            targetScreen = info_screen;
            break;
        case STATE_SETTINGS:
            targetScreen = settings_screen;
            break;
        default:
            targetScreen = main_screen;
    }
    
    lv_scr_load(targetScreen);
}

void buttonHandlerTask(void *ptr) {
    const BoardsConfigure_t *boards = amoled.getBoardsConfigure();
    while (1) {
        for (int i = 0; i < boards->buttonNum; ++i) {
            button[i].check();
        }
        delay(5);
    }
    vTaskDelete(NULL);
}

void buttonHandleEvent(AceButton *button, uint8_t eventType, uint8_t buttonState) {
    uint8_t id;
    const BoardsConfigure_t *boards = amoled.getBoardsConfigure();
    id = amoled.getBoardID();

    switch (eventType) {
        case AceButton::kEventClicked:
            if (button->getId() == 0) {
                // Cycle through screens
                currentState = (GUI_STATE)((currentState + 1) % 4);
                switchToScreen(currentState);
            }
            break;
        case AceButton::kEventLongPressed:
            Serial.println("Enter sleep!");
            enterSleepMode();
            break;
        default:
            break;
    }
}

void enterSleepMode() {
    Serial.println("Entering sleep mode...");

    // Delete any running tasks
    if (vUpdateHAStateTaskHandler) {
        vTaskDelete(vUpdateHAStateTaskHandler);
    }
    if (vUpdateSensorTaskHandler) {
        vTaskDelete(vUpdateSensorTaskHandler);
    }

    sleep_flag = true;

    // Disconnect WiFi
    WiFi.disconnect();
    WiFi.removeEvent(WiFiEvent);
    WiFi.mode(WIFI_OFF);

    const BoardsConfigure_t *boards = amoled.getBoardsConfigure();
    uint8_t id = amoled.getBoardID();

    bool touchpad_sleep = true;
    if (id == LILYGO_AMOLED_191 || id == LILYGO_AMOLED_191_SPI) {
        // 1.91 inch screen needs special handling
        touchpad_sleep = false;
    }
    amoled.sleep(touchpad_sleep);    // Turn off NeoPixels if available
    if (boards->pixelsPins != -1 && pixels) {
        pixels->clear();
        pixels->show();
    }

    // Handle PMU sleep for specific boards
    if (boards->pmu && id == LILYGO_AMOLED_147) {
        amoled.enableSleep();
        amoled.clearPMU();
        amoled.enableWakeup();
        esp_sleep_enable_timer_wakeup(60 * 1000000ULL); // 60 seconds
    } else {
        // Set BOOT button as wakeup source
        esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, LOW);
    }

    Wire.end();
    
    Serial.println("Sleep Start!");
    delay(1000);
    esp_deep_sleep_start();
    Serial.println("This place will never print!");
}

void WiFiEvent(WiFiEvent_t event) {
    Serial.printf("[WiFi-event] event: %d\n", event);

    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            Serial.print("WiFi connected! IP address: ");
            Serial.println(WiFi.localIP());
              // Update WiFi status label
            if (wifi_label) {
                lv_label_set_text(wifi_label, "WiFi: Connected");
            }
            
            // Configure time
            configTzTime("UTC", "pool.ntp.org", "time.nist.gov");
            
            // Start tasks for Home Assistant communication and sensor updates
            if (!vUpdateHAStateTaskHandler) {
                xTaskCreate(updateHAStateTask, "ha_state", 10 * 1024, NULL, 12, &vUpdateHAStateTaskHandler);
            }
            if (!vUpdateSensorTaskHandler) {
                xTaskCreate(updateSensorTask, "sensors", 10 * 1024, NULL, 12, &vUpdateSensorTaskHandler);
            }
            break;
              case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            Serial.println("WiFi lost connection");
            if (wifi_label) {
                lv_label_set_text(wifi_label, "WiFi: Disconnected");
            }
            break;
            
        default:
            break;
    }
}

void updateHAStateTask(void *ptr) {
    int retryCount = 0;
    unsigned long lastFastUpdate = 0;
    while (1) {
        delay(1000);  // Short delay for initial check

        if (!WiFi.isConnected()) {
            Serial.println("WiFi not connected, waiting before checking HA state");
            delay(5000);
            continue;
        }

        // Determine update frequency based on retry count
        unsigned long now = millis();
        unsigned long updateInterval = (retryCount > 3) ? 30000 : 5000; // Back off if failing

        if (now - lastFastUpdate < updateInterval) {
            delay(1000);
            continue;
        }

        lastFastUpdate = now;

        if (xSemaphoreTake(xWiFiLock, pdMS_TO_TICKS(5000)) == pdTRUE) {
            Serial.println("Updating Home Assistant state...");
            
            bool success = getHAState();
            
            if (success) {
                retryCount = 0;
                // Update UI only if light_switch is valid
                if (light_switch) {
                    if (lightState) {
                        lv_obj_add_state(light_switch, LV_STATE_CHECKED);
                    } else {
                        lv_obj_clear_state(light_switch, LV_STATE_CHECKED);
                    }
                } else {
                    Serial.println("Warning: light_switch is NULL, can't update UI");
                }
            } else {
                retryCount++;
                Serial.printf("Failed to get HA state, retry count: %d\n", retryCount);
            }
              // Update time display
            struct tm timeinfo;
            if (getLocalTime(&timeinfo)) {
                char timeStr[9];
                strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
                if (time_label) {
                    lv_label_set_text(time_label, timeStr);
                }
                
                char dateStr[20];
                strftime(dateStr, sizeof(dateStr), "%Y-%m-%d %a", &timeinfo);
                if (date_label) {
                    lv_label_set_text(date_label, dateStr);
                }
            } else {
                Serial.println("Failed to obtain time");
            }
            
            xSemaphoreGive(xWiFiLock);
        }
    }
}

void updateSensorTask(void *ptr) {
    while (1) {
        delay(10000);  // Update every 10 seconds initially, then less frequently
        static unsigned long lastFullUpdate = 0;
        unsigned long now = millis();

        if (!WiFi.isConnected()) {
            delay(1000);
            continue;
        }

        // Try to take the WiFi semaphore, but don't block forever
        if (xSemaphoreTake(xWiFiLock, pdMS_TO_TICKS(5000)) == pdTRUE) {
            // For a real implementation, you would fetch sensor data from Home Assistant here
            // For now, we'll just simulate changing values
            Serial.println("Updating sensor values");
            
            // Only update values every 30 seconds to reduce flicker
            if (now - lastFullUpdate > 30000) {
                temperature += (random(10) - 5) / 10.0;
                if (temperature < 15) temperature = 15;
                if (temperature > 30) temperature = 30;
                
                humidity += (random(10) - 5) / 10.0;
                if (humidity < 30) humidity = 30;
                if (humidity > 70) humidity = 70;
                
                lastFullUpdate = now;
            }
            
            // Update display if labels exist
            if (temperature_label != NULL) {
                char tempStr[10];
                sprintf(tempStr, "%.1f °C", temperature);
                lv_label_set_text(temperature_label, tempStr);
            }
            
            if (humidity_label != NULL) {
                char humStr[10];
                sprintf(humStr, "%.1f %%", humidity);
                lv_label_set_text(humidity_label, humStr);
            }
            
            xSemaphoreGive(xWiFiLock);
        } else {
            Serial.println("Could not acquire WiFi lock for sensor update");
        }
        
        // Gradually slow down updates to save power
        delay(20000);  // Now wait every 30 seconds (10 + 20)
    }
}

void toggleLight() {
    if (!WiFi.isConnected()) {
        Serial.println("WiFi not connected, cannot toggle light");
        return;
    }

    HTTPClient http;
    String apiUrl = String(HOME_ASSISTANT_URL) + "/api/services/light/" + (lightState ? "turn_on" : "turn_off");
    Serial.print("Sending request to: ");
    Serial.println(apiUrl);
    
    http.begin(apiUrl);
    http.addHeader("Authorization", "Bearer " + String(HOME_ASSISTANT_TOKEN));
    http.addHeader("Content-Type", "application/json");
    
    String payload = "{\"entity_id\":\"" + String(LIGHT_ENTITY_ID) + "\"}";
    Serial.print("Payload: ");
    Serial.println(payload);
    
    int httpResponseCode = http.POST(payload);
    
    handleHTTPResponse(http, httpResponseCode, "Toggling light");
    
    http.end();
}

void setBrightness(int brightness) {
    if (!WiFi.isConnected()) {
        Serial.println("Cannot set brightness: WiFi not connected");
        return;
    }

    HTTPClient http;
    String apiUrl = String(HOME_ASSISTANT_URL) + "/api/services/light/turn_on";
    Serial.print("Setting brightness via: ");
    Serial.println(apiUrl);
    
    http.begin(apiUrl);
    http.addHeader("Authorization", "Bearer " + String(HOME_ASSISTANT_TOKEN));
    http.addHeader("Content-Type", "application/json");
    
    String payload = "{\"entity_id\":\"" + String(LIGHT_ENTITY_ID) + "\",\"brightness_pct\":" + String(brightness) + "}";
    Serial.print("Payload: ");
    Serial.println(payload);
    
    int httpResponseCode = http.POST(payload);
    
    handleHTTPResponse(http, httpResponseCode, "Setting brightness");
    
    http.end();
}

bool getHAState() {
    if (!WiFi.isConnected()) {
        Serial.println("WiFi not connected, cannot get HA state");
        return false;
    }

    HTTPClient http;
    http.begin(String(HOME_ASSISTANT_URL) + "/api/states/" + String(LIGHT_ENTITY_ID));
    http.addHeader("Authorization", "Bearer " + String(HOME_ASSISTANT_TOKEN));
    
    int httpResponseCode = http.GET();
    bool success = false;
    
    if (httpResponseCode > 0) {
        String response = http.getString();
        Serial.println("Response received from Home Assistant");
          // Parse JSON response - use a larger buffer for safety
        DynamicJsonDocument doc(2048);
        DeserializationError error = deserializeJson(doc, response);
        
        if (!error) {
            const char* state = doc["state"];
            if (state) {
                lightState = (strcmp(state, "on") == 0);
                Serial.print("Light state: ");
                Serial.println(lightState ? "ON" : "OFF");
                
                // Get brightness if available
                if (doc["attributes"].containsKey("brightness")) {
                    int brightness = doc["attributes"]["brightness"];
                    // Convert from 0-255 to 0-100 scale
                    brightnessValue = map(brightness, 0, 255, 0, 100);
                    lv_slider_set_value(brightness_slider, brightnessValue, LV_ANIM_OFF);
                    Serial.print("Light brightness: ");
                    Serial.println(brightnessValue);
                }
                
                success = true;
            }
        } else {
            Serial.print("JSON parsing failed! Error: ");
            Serial.println(error.c_str());
        }
    } else {
        Serial.print("Error getting HA state. HTTP Error code: ");
        Serial.println(httpResponseCode);
    }
    
    http.end();
    return success;
}

bool handleHTTPResponse(HTTPClient &http, int httpResponseCode, const char* operation) {
    if (httpResponseCode > 0) {
        String response = http.getString();
        Serial.printf("%s successful, HTTP code: %d\n", operation, httpResponseCode);
        Serial.println(response);
        return true;
    } else {
        Serial.printf("Error in %s. HTTP error code: %d\n", operation, httpResponseCode);
        switch (httpResponseCode) {
            case -1:
                Serial.println("Connection refused or timeout");
                break;
            case -2:
                Serial.println("SSL client error");
                break;
            case -3:
                Serial.println("Server certificate verification failed");
                break;
            case -4:
                Serial.println("Generic connection error");
                break;
            case -11:
                Serial.println("Connection failed (possibly wrong protocol)");
                break;
            default:
                Serial.printf("Unknown error code: %d\n", httpResponseCode);
                break;
        }
        return false;
    }
}