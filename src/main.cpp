#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESPAsyncUDP.h>
#include <SimplePortal.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <PubSubClient.h>
#include <ESP8266Ping.h>
#include <EncButton.h>
#include <EEPROM.h>

// Pin definitions
#define LED_PIN 2      // LED pin
#define CONTROL_PIN 12 // D6
#define BUTT_PIN 14    // D5
#define NTC_PIN A0

// Timeouts and periods
#define SAVE_TERMO_TIMEOUT 3600000 // 1 hour
#define LED_BLINK_PERIOD 1000
#define DATA_SEND_PERIOD 1000
#define TRY_CONN_TIMES 140
#define MQTT_CONN_PERIOD 3000 // 3 sec
#define HOT_VAL 100           // 810
#define TERMO_VAL 80          // 660
#define TERMO_HYSTERESIS 4
#define AVEARGE_COUNTS 10
#define NTP_CORR_TIME_PERIOD 600000 // NTP time correction every 10 min (600 000 ms)
#define NTP_TIMEZONE 3              // Belarus

// Button initialization
Button buttAction(BUTT_PIN, INPUT_PULLUP, LOW);

// WiFi and MQTT clients
WiFiClient myWifi;
PubSubClient client(myWifi);

// UDP and NTP clients
AsyncUDP udp;
WiFiUDP ntpUDP;

// NTP client initialization
NTPClient timeClient(ntpUDP, "europe.pool.ntp.org", 3600 * NTP_TIMEZONE, NTP_CORR_TIME_PERIOD);

// Mode enumeration
enum Mode
{
    OFF,
    HOT,
    TERM
};

// Alarm type enumeration
enum alarmType
{
    ALM_HOT,
    ALM_TERM
};

// Global variables
uint8_t ntpHour = 0;
uint8_t ntpMinute = 0;
uint8_t ntpSecond = 0;

uint8_t modeType = OFF, tryingCnt = 0;
uint16_t an = 0, anOld = 0;
uint32_t _tmTerm = 0, _tmLed = 0, _tmTmp = 0, _tmInf = 0, _tmMqttConn = 0, _tmNtpCorr = 0, _tmSwClock = 0;

uint16_t setTempVal = TERMO_VAL; // Set temperature value on start

bool partTerm = false, doMultipleTime = true, debugSerial = false, debugUdp = false, wifiOnline = false, hotArming = false, almSrabotFlag = false;

// Configuration structure
struct Config
{
    char ssid[20];
    char pass[20];
    char mserver[20];
    uint16_t port;
    char mlogin[20];
    char mpass[20];
    char topctrl[20];
    char topinfo[20];
} cf;

struct alarm
{
    bool enAlm;
    uint8_t almType;
    uint8_t almHour;
    uint8_t almMin;
    uint8_t almSec;
} al;

// Function prototypes
void checkChipID();
void setHeater(bool);
void setAlarmOff();
void setOff();
void setHot();
void setTermo();
void setReset();
void ledHandler();
void buttHandler();
void execHandler();
void wifiManagerHandler();
void setWIFIConfig(const char *ssid, const char *pass);
void connectToWiFi();
void eepromGetSetup();
void eepromStoreSetup();
void eepromGetAl();
void eepromStoreAl();
void mqttManagerHandler();
void callback(const MQTT::Publish &pub);
void sendDataUDP(String data);
void sendAlleepromDataUdp();
void getTempData();
void ntpCorrectTimeHandler();
void softwareClockHandler();
void calculateAlarmTime(String p);
void checkAlarm();
// bool pingGoogle();

void setup(void)
{
    // Check if the button is pressed during startup
    if (!digitalRead(BUTT_PIN))
        hotArming = true;

    // Initialize serial for debugging
    if (debugSerial)
    {
        Serial.begin(115200);

        delay(10);
    }

    // Check chip ID
    checkChipID();

    // Initialize EEPROM
    // EEPROM.begin(150); // for 122 bytes in structure
    EEPROM.begin(sizeof(cf) + sizeof(al) + 1); // for 122 bytes in structure

    eepromGetSetup();

    // for test
    // al.enAlm = true;
    // al.almType = 1;
    // al.almHour = 15;
    // al.almMin = 45;
    // eepromStoreAl();

    eepromGetAl();

    // Set pin modes
    pinMode(LED_PIN, OUTPUT);
    pinMode(CONTROL_PIN, OUTPUT);

    // Set heater to off by default
    setHeater(false);

    // Set WiFi configuration
    setWIFIConfig(cf.ssid, cf.pass);

    // Initialize OTA, NTP, and MQTT if internet is available
    if (wifiOnline)
    {
        client.set_server(cf.mserver, cf.port);
        ArduinoOTA.begin();
        timeClient.begin();

        if (debugUdp)
            sendAlleepromDataUdp();
    }

    _tmMqttConn = millis() + MQTT_CONN_PERIOD;    // for mqtt immediate reaction if needed
    _tmNtpCorr = millis() + NTP_CORR_TIME_PERIOD; // for ntp correction
}

void loop(void)
{
    if (wifiOnline)
    {
        ArduinoOTA.handle();
        timeClient.update();
    }

    getTempData();

    buttHandler();
    ledHandler();
    execHandler();
    wifiManagerHandler();
    mqttManagerHandler();
    ntpCorrectTimeHandler();
    softwareClockHandler();
}

void getTempData()
{
    if (millis() - _tmTmp > 100) // get temp every 0.1 sec
    {
        for (size_t i = 0; i < AVEARGE_COUNTS; i++)
            an += analogRead(NTC_PIN);

        an = an / AVEARGE_COUNTS / 8; // average degrees value and convert to temperature

        if (an != anOld)
        {
            anOld = an;

            if (debugSerial)
                Serial.println("adc_d: " + String(an));
        }

        _tmTmp = millis();
    }
}

void ntpCorrectTimeHandler()
{
    if (millis() - _tmNtpCorr > NTP_CORR_TIME_PERIOD)
    {
        _tmNtpCorr = millis(); // reset correction time
        timeClient.update();

        // Используем sscanf для более удобного разбора времени
        String formattedTime = timeClient.getFormattedTime();
        sscanf(formattedTime.c_str(), "%2hhu:%2hhu:%2hhu", &ntpHour, &ntpMinute, &ntpSecond);

        _tmSwClock = millis(); // reset software clock time
    }
}

void softwareClockHandler()
{
    if (millis() - _tmSwClock > 1000)
    {
        _tmSwClock = millis();

        if (++ntpSecond >= 60) // rise and compare seconds
        {
            ntpSecond = 0;

            if (++ntpMinute >= 60) // rise and compare minutes
            {
                ntpMinute = 0;

                if (++ntpHour >= 24) // rise and compare hours
                    ntpHour = 0;
            }
        }

        checkAlarm(); // time to execute alarm function, or not?
    }
}

void checkAlarm()
{
    if (al.enAlm)
    {
        if (ntpHour == al.almHour && ntpMinute == al.almMin && ntpSecond == al.almSec) // if time to activate alarm
        {
            almSrabotFlag = true;

            switch (al.almType)
            {
            case ALM_HOT:
                setTempVal = HOT_VAL;

                setHot();

                break;
            case ALM_TERM:
                setTempVal = TERMO_VAL;

                setTermo();

                break;
            }
        }
    }
}

void buttHandler()
{
    if (hotArming)
    {
        hotArming = false;
        setTempVal = HOT_VAL;

        setHot();
    }
    else
    {
        buttAction.tick();

        if (buttAction.hasClicks())
        {
            uint8_t btn = buttAction.getClicks();

            switch (btn)
            {
            case 1:
                setTempVal = HOT_VAL;

                setHot();

                break;
            case 2:
                setTempVal = TERMO_VAL;

                setTermo();

                break;
            case 7:
                setReset();

                break;
            }
        }

        if (buttAction.hold())
            setOff();
    }
}

void ledHandler()
{
    switch (modeType)
    {
    case OFF:
        digitalWrite(LED_PIN, HIGH);

        break;
    case HOT:
        digitalWrite(LED_PIN, LOW);

        break;
    case TERM:
        if (millis() - _tmLed > LED_BLINK_PERIOD)
        {
            digitalWrite(LED_PIN, !digitalRead(LED_PIN));
            _tmLed = millis();
        }

        break;
    }
}

void execHandler()
{
    switch (modeType)
    {
    case OFF:
        setHeater(LOW);

        break;
    case HOT:
        if (an < setTempVal)
            setHeater(HIGH);
        else
            setOff();

        break;
    case TERM:
        if (millis() - _tmTerm > SAVE_TERMO_TIMEOUT)
            setOff();

        if (!partTerm)
        {
            if (an < setTempVal)
                setHeater(HIGH);
            else
            {
                setHeater(LOW);
                partTerm = !partTerm;
            }
        }
        else
        {
            if (an < (setTempVal - TERMO_HYSTERESIS))
                partTerm = !partTerm;
        }

        break;
    }
}

void setHeater(bool b)
{
    digitalWrite(CONTROL_PIN, b);
}

void setHot()
{
    if (debugSerial)
        Serial.println("Heater: Hot");

    modeType = HOT;
    partTerm = false;
}

void setTermo()
{
    if (debugSerial)
        Serial.println("Heater: Termo");

    modeType = TERM;
    partTerm = false;

    _tmTerm = millis();
}

void setOff()
{
    if (debugSerial)
        Serial.println("Heater: Off");

    modeType = OFF;
    partTerm = false;
}

void setAlarmOff()
{
    if (debugSerial)
        Serial.println("Heater: Off");

    almSrabotFlag = false;
    al.enAlm = false;

    eepromStoreAl();
}

void setReset()
{
    if (debugSerial)
        Serial.println("Set: Reset");

    strncpy(cf.ssid, "SSID", sizeof(cf.ssid));
    strncpy(cf.pass, "PASS", sizeof(cf.pass));
    strncpy(cf.mserver, "SERVER", sizeof(cf.mserver));
    strncpy(cf.mlogin, "LOGIN", sizeof(cf.mlogin));
    strncpy(cf.mpass, "MPASS", sizeof(cf.mpass));
    strncpy(cf.topctrl, "TOPCTRL", sizeof(cf.topctrl));
    strncpy(cf.topinfo, "TOPINFO", sizeof(cf.topinfo));
    cf.port = 0;

    eepromStoreSetup();

    ESP.restart();
}

void setWIFIConfig(const char *ssid, const char *pass)
{
    if (debugSerial)
        Serial.print("\nConnecting to WiFi");

    WiFi.begin(ssid, pass);
    wifiOnline = true;

    connectToWiFi();
}

void connectToWiFi()
{
    while (WiFi.status() != WL_CONNECTED)
    {
        if (debugSerial)
            Serial.print(".");

        digitalWrite(LED_PIN, !digitalRead(LED_PIN));
        tryingCnt++;

        if (tryingCnt == TRY_CONN_TIMES)
        {
            wifiOnline = false;
            break;
        }

        delay(250);
    }

    tryingCnt = 0;
    digitalWrite(LED_PIN, HIGH); // forced turn off LED after connecting blinking

    if (debugSerial)
    {
        Serial.print("\nIP:");
        Serial.println(WiFi.localIP());
    }
}

void wifiManagerHandler()
{
    if (doMultipleTime && (WiFi.status() != WL_CONNECTED))
    {
        doMultipleTime = false;
        wifiOnline = false;

        portalStart();

        if (debugSerial)
            Serial.println("Portal Started");
    }

    if (portalTick())
    {
        if (debugSerial)
            Serial.println(portalStatus());

        if (portalStatus() == SP_SUBMIT)
        {
            strncpy(cf.ssid, portalCfg.SSID, sizeof(cf.ssid));
            strncpy(cf.pass, portalCfg.pass, sizeof(cf.pass));
            strncpy(cf.mserver, portalCfg.mserver, sizeof(cf.mserver));
            strncpy(cf.mlogin, portalCfg.mlogin, sizeof(cf.mlogin));
            strncpy(cf.mpass, portalCfg.mpass, sizeof(cf.mpass));
            strncpy(cf.topctrl, portalCfg.topctrl, sizeof(cf.topctrl));
            strncpy(cf.topinfo, portalCfg.topinfo, sizeof(cf.topinfo));

            String s = portalCfg.mport;
            cf.port = s.toInt();

            if (debugSerial)
            {
                Serial.print(cf.ssid);
                Serial.print('\t');
                Serial.print(cf.pass);
                Serial.print('\t');
                Serial.print(cf.mserver);
                Serial.print('\t');
                Serial.print(cf.port);
                Serial.print('\t');
                Serial.print(cf.mlogin);
                Serial.print('\t');
                Serial.print(cf.mpass);
                Serial.print('\t');
                Serial.print(cf.topctrl);
                Serial.print('\t');
                Serial.println(cf.topinfo);
            }

            eepromStoreSetup();

            if (debugUdp)
                sendAlleepromDataUdp();

            if (debugSerial)
                Serial.println("Portal Ended");

            ESP.restart();
        }
        else if (portalStatus() == SP_EXIT)
        {
            if (debugSerial)
                Serial.println("System Reset");

            ESP.restart();
        }
    }
}

void callback(const MQTT::Publish &pub)
{
    if (debugSerial)
    {
        Serial.print("Data:");
        Serial.print(pub.payload_string());
        Serial.print('\t');
        Serial.print("Topic:");
        Serial.println(pub.topic());
    }

    if (debugUdp)
    {
        sendDataUDP("Topic: " + pub.topic() + "\tData: " + pub.payload_string());
    }

    if (String(pub.topic()) == cf.topctrl)
    {
        String payload = pub.payload_string();

        if (payload[0] == 't') // termo mode
        {
            payload.remove(0, 1);
            setTempVal = payload.toInt();

            setTermo();
        }
        else if (payload[0] == 'x') // off mode
        {
            setOff(); // set off heater
        }
        else if (payload[0] == 'h') // alarm mode
        {
            if (payload[1] == '0')
            {
                setAlarmOff(); // set off alarm whith save al data

                return;
            }
            else if (payload[1] == '1') // HOT alarm
                al.almType = ALM_HOT;
            else if (payload[1] == '2') // TERM alarm
                al.almType = ALM_TERM;

            calculateAlarmTime(payload);

            al.enAlm = true; // alarm enable

            eepromStoreAl();
        }
        else
        {
            setTempVal = payload.toInt();

            setHot();
        }
    }
}

void calculateAlarmTime(String p)
{
    p.remove(0, 3); // remove first 3 characters

    int separatorIndexFirst = p.indexOf(':'); // find the index of the ':' separator
    // int separatorIndexSecond = p.indexOf(','); // find the index of the ':' separator (for second)

    if (separatorIndexFirst != -1)
    {
        al.almHour = p.substring(0, separatorIndexFirst).toInt(); // Convert to integer
        al.almMin = p.substring(separatorIndexFirst + 1).toInt();
        al.almSec = 0;
        // al.almSec = p.substring(separatorIndexSecond + 1).toInt();
    }
}

void mqttManagerHandler()
{
    if (WiFi.status() == WL_CONNECTED)
    {
        if (!client.connected() && (millis() - _tmMqttConn > MQTT_CONN_PERIOD))
        {
            if (debugSerial)
                Serial.print("Connecting to MQTT server...");

            if (client.connect(MQTT::Connect("teapotClient")
                                   .set_auth(cf.mlogin, cf.mpass)))
            {
                if (debugSerial)
                    Serial.println("Connected");

                client.subscribe(cf.topctrl);
                client.set_callback(callback);
            }
            else
            {
                if (debugSerial)
                    Serial.println("Could not connect");
            }

            _tmMqttConn = millis();
        }
        else
        {
            client.loop();

            if (millis() - _tmInf > DATA_SEND_PERIOD)
            {
                client.publish(cf.topinfo, String(an) + "," + String(setTempVal) + "," + String(modeType) + "," + String(al.enAlm) + "," + String(ntpHour) + ":" + String(ntpMinute));

                if (debugUdp)
                    sendDataUDP(String(an));

                _tmInf = millis();
            }
        }
    }
}

void eepromGetSetup()
{
    EEPROM.get(0, cf);

    delay(5);
}

void eepromStoreSetup()
{
    EEPROM.put(0, cf);
    EEPROM.commit();
}

void eepromGetAl()
{
    EEPROM.get(sizeof(cf) + 1, al);

    delay(5);
}

void eepromStoreAl()
{
    EEPROM.put(sizeof(cf) + 1, al);
    EEPROM.commit();
}

void sendDataUDP(String data)
{
    udp.broadcastTo(data.c_str(), 2255); // Port 2255 is UDP Broadcasting port

    delay(10);
}

void sendAlleepromDataUdp()
{
    sendDataUDP(cf.ssid);
    sendDataUDP(cf.pass);
    sendDataUDP(cf.mserver);
    sendDataUDP(String(cf.port));
    sendDataUDP(cf.mlogin);
    sendDataUDP(cf.mpass);
    sendDataUDP(cf.topctrl);
    sendDataUDP(cf.topinfo);
}

void checkChipID()
{
    uint32_t cID = ESP.getChipId();

    while (!(cID == 3396484))
    {
        if (debugSerial)
        {
            Serial.println();
            Serial.println("Chip fail");
        }

        delay(1000);
    }

    if (debugSerial)
        Serial.println("\nChip pass");
}

// bool pingGoogle()
// {
//     return Ping.ping("8.8.8.8", 1);
// }
