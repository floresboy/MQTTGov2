#include <arduino.h>

#include <WiFi.h>
#include <MQTT.h>
#include <SPIFFS.h>
#include <WiFiSettings.h>
#include <MHZ19.h>
#include <ArduinoOTA.h>
#include <SPI.h>
#include <TFT_eSPI.h>

#include <list>
#include <MQTT_strings.h>

#include <display-cmd.h>

#define LANGUAGE "nl"
MQTTLanguage::Texts T;

enum Driver { AQC, MHZ };
Driver          driver;
MQTTClient      mqtt;
HardwareSerial  hwserial1(1);

MHZ19           mhz;

const int       pin_portalbutton = 35;
const int       pin_demobutton   = 0;
const int       pin_backlight    = 4;
const int       pin_sensor_rx    = 27;
const int       pin_sensor_tx    = 26;
const int       pin_pcb_ok       = 12;   // pulled to GND by PCB trace
int             mhz_co2_init     = 410;  // magic value reported during init

// Configuration via WiFiSettings
unsigned long   mqtt_interval;
bool            ota_enabled;
int             co2_warning;
int             co2_critical;
int             co2_blink;
String          mqtt_topic;
String          mqtt_template;
bool            add_units;
bool            wifi_enabled;
bool            mqtt_enabled;
int             max_failures;

void retain(const String& topic, const String& message) {
    Serial.println("MQTTGo v2 Pub-ing ret. message now...");
    Serial.printf("topic: %s payload: %s\n", topic.c_str(), message.c_str());
    mqtt.publish(topic, message, true, 0);
}



void display_ppm(int ppm) {
    int fg, bg;
    if (ppm >= co2_critical) {
        fg = TFT_WHITE;
        bg = TFT_RED;
    } else if (ppm >= co2_warning) {
        fg = TFT_BLACK;
        bg = TFT_YELLOW;
    } else {
        fg = TFT_GREEN;
        bg = TFT_BLACK;
    }

    if (ppm >= co2_blink && millis() % 2000 < 1000) {
        std::swap(fg, bg);
    }
    display_big(String(ppm), fg, bg);
}



void ppm_demo() {
    display_big("demo!");
    delay(3000);
    display_logo();
    delay(1000);
    int buttoncounter = 0;
    for (int p = 400; p < 1200; p++) {
        display_ppm(p);
        if (button(pin_demobutton)) {
            display_logo();
            delay(500);
            return;
        }
    }
    display_logo();
    delay(5000);
}

void panic(const String& message) {
    display_big(message, TFT_RED);
    delay(5000);
    ESP.restart();
}

bool button(int pin) {
    if (digitalRead(pin)) return false;
    unsigned long start = millis();
    while (!digitalRead(pin)) {
        if (millis() - start >= 50) display_big("");
    }
    return millis() - start >= 50;
}

void check_portalbutton() {
    if (button(pin_portalbutton)) WiFiSettings.portal();
}

void check_demobutton() {
    if (button(pin_demobutton)) ppm_demo();
}

void check_buttons() {
    check_portalbutton();
    check_demobutton();
}

void setup_ota() {
    ArduinoOTA.setHostname(WiFiSettings.hostname.c_str());
    ArduinoOTA.setPassword(WiFiSettings.password.c_str());
    ArduinoOTA.onStart(   []()              { display_big("OTA", TFT_BLUE); });
    ArduinoOTA.onEnd(     []()              { display_big("OTA done", TFT_GREEN); });
    ArduinoOTA.onError(   [](ota_error_t e) { display_big("OTA failed", TFT_RED); });
    ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
        String pct { (int) ((float) p / t * 100) };
        display_big(pct + "%");
    });
    ArduinoOTA.begin();
}

void connect_mqtt() {
    if (mqtt.connected()) return;  // already/still connected

    static int failures = 0;
    if (mqtt.connect(WiFiSettings.hostname.c_str())) {
        failures = 0;
    } else {
        failures++;
        if (failures >= max_failures) panic(T.error_mqtt);
    }
}

void flush(Stream& s, int limit = 20) {
    // .available() sometimes stays true (why?), hence the limit

    s.flush();  // flush output
    while(s.available() && --limit) s.read();  // flush input
}



int get_co2() {
    // <0 means read error, 0 means still initializing, >0 is PPM value

    //if (driver == AQC) return aqc_get_co2();
    // if (driver == MHZ) return mhz_get_co2();
    return(2304);

    // Should be unreachable
    panic(T.error_driver);
    return -1;  // suppress warning
}


// ******************* SetUp ************************************** SetUp ************************************* SetUp *******************

void setup() {
    Serial.begin(115200);
    Serial.println("MQTTGo v2 start");

    display_init();

    MQTTLanguage::select(T, LANGUAGE);

    if (!SPIFFS.begin(false)) {
        display_lines(T.first_run, TFT_MAGENTA);
        if (!SPIFFS.format()) {
            display_big(T.error_format, TFT_RED);
            delay(20*1000);
        }
    }

    pinMode(pin_portalbutton,   INPUT_PULLUP);
    pinMode(pin_demobutton,     INPUT_PULLUP);
    // pinMode(pin_pcb_ok,         INPUT_PULLUP);
    pinMode(pin_backlight,      OUTPUT);

    WiFiSettings.hostname = "HiveMq-";
    WiFiSettings.language = LANGUAGE;
    WiFiSettings.begin();
    MQTTLanguage::select(T, WiFiSettings.language);

    Serial.println("MQTTGo v2 Display logo");
    display_logo();
    delay(2000);



    for (auto& str : T.portal_instructions[0]) {
        str.replace("{ssid}", WiFiSettings.hostname);
    }

    wifi_enabled  = WiFiSettings.checkbox("operame_wifi", false, T.config_wifi);
    // ota_enabled   = WiFiSettings.checkbox("operame_ota", false, T.config_ota) && wifi_enabled;

    WiFiSettings.heading("CO2-niveaus");
    co2_warning   = WiFiSettings.integer("operame_co2_warning", 400, 5000, 700, T.config_co2_warning);
    co2_critical  = WiFiSettings.integer("operame_co2_critical",400, 5000, 800, T.config_co2_critical);
    co2_blink     = WiFiSettings.integer("operame_co2_blink",   800, 5000, 800, T.config_co2_blink);

    WiFiSettings.heading("MQTT");
    mqtt_enabled  = WiFiSettings.checkbox("operame_mqtt", false, T.config_mqtt) && wifi_enabled;
    String server = WiFiSettings.string("mqtt_server", 64, "", T.config_mqtt_server);
    int port      = WiFiSettings.integer("mqtt_port", 0, 65535, 1883, T.config_mqtt_port);
    max_failures  = WiFiSettings.integer("operame_max_failures", 0, 1000, 10, T.config_max_failures);
    mqtt_topic  = WiFiSettings.string("operame_mqtt_topic", WiFiSettings.hostname, T.config_mqtt_topic);
    mqtt_interval = 1000UL * WiFiSettings.integer("operame_mqtt_interval", 10, 3600, 10, T.config_mqtt_interval);
    mqtt_template = WiFiSettings.string("operame_mqtt_template", "{} PPM", T.config_mqtt_template);
    WiFiSettings.info(T.config_template_info);

    WiFiSettings.onConnect = [] {
        display_big(T.connecting, TFT_BLUE);
        check_portalbutton();
        return 50;
    };
    WiFiSettings.onFailure = [] {
        display_big(T.error_wifi, TFT_RED);
        delay(2000);
    };
    static int portal_phase = 0;
    static unsigned long portal_start;
    WiFiSettings.onPortal = [] {
        if (ota_enabled) setup_ota();
        portal_start = millis();
    };
    WiFiSettings.onPortalView = [] {
        if (portal_phase < 2) portal_phase = 2;
    };
    WiFiSettings.onConfigSaved = [] {
        portal_phase = 3;
    };
    WiFiSettings.onPortalWaitLoop = [] {
        if (WiFi.softAPgetStationNum() == 0) portal_phase = 0;
        else if (! portal_phase) portal_phase = 1;

        display_lines(T.portal_instructions[portal_phase], TFT_WHITE, TFT_BLUE);

        if (portal_phase == 0 && millis() - portal_start > 10*60*1000) {
            panic(T.error_timeout);
        }

        if (ota_enabled) ArduinoOTA.handle();
        if (button(pin_portalbutton)) ESP.restart();
    };

    if (wifi_enabled) WiFiSettings.connect(false, 15);

    mqtt_enabled = true;
    static WiFiClient wificlient;
    if (mqtt_enabled) mqtt.begin(server.c_str(), port, wificlient);
    
    ota_enabled = false;
    if (ota_enabled) setup_ota();
}


// ******************* Loop ************************************** Loop ************************************** Loop *******************

#define every(t) for (static unsigned long _lasttime; (unsigned long)((unsigned long)millis() - _lasttime) >= (t); _lasttime = millis())

void loop() {
    static int co2;

    every(5000) {
        co2 = get_co2();
        Serial.print("MQTTGo v2 Get Co2 value : ");
        Serial.println(co2);
    }

    every(50) {
        if (co2 < 0) {
            display_big(T.error_sensor, TFT_RED);
        } else if (co2 == 0) {
            display_big(T.wait);
        } else {
            // Serial.println("MQTTGo v2 Update display now...");
            // some MH-Z19's go to 10000 but the display has space for 4 digits
            display_ppm(co2 > 9999 ? 9999 : co2);
        }
    }

    if (mqtt_enabled) {
        mqtt.loop();
        every(mqtt_interval) {
            if (co2 <= 0) break;
            connect_mqtt();
            String message = mqtt_template;
            message.replace("{}", String(co2));
            retain(mqtt_topic, message);
        }
    }

    // if (ota_enabled) ArduinoOTA.handle();
    check_buttons();
}
