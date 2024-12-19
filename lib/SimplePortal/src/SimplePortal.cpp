#include "SimplePortal.h"
static DNSServer _SP_dnsServer;
#ifdef ESP8266
static ESP8266WebServer _SP_server(80);
#else
static WebServer _SP_server(80);
#endif

const char SP_connect_page[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style type="text/css">
        input[type="text"] {
            margin-bottom: 8px;
            font-size: 20px;
            display: block;
            background-color: #333;
            color: #fff;
            border: none;
            padding: 10px;
        }
        input[type="submit"] {
            width: 180px;
            height: 60px;
            margin-bottom: 8px;
            font-size: 20px;
            display: block;
            background-color: #333;
            color: #fff;
            border: none;
            padding: 10px;
        }
        body {
            background-color: #222;
            color: #fff;
        }
        h3 {
            color: #fff;
        }
        label {
            color: #fff;
        }
    </style>
</head>
<body>
    <center>
        <form action="/connect" method="POST">
            <h3>WiFi settings</h3>
            <label for="ssid">SSID:</label>
            <input type="text" name="ssid" id="ssid" placeholder="SSID">
            <label for="pass">Pass:</label>
            <input type="text" name="pass" id="pass" placeholder="Pass">
            <h3>MQTT settings</h3>
            <label for="mserver">mServer:</label>
            <input type="text" name="mserver" id="mserver" placeholder="mServer">
            <label for="mport">mPort:</label>
            <input type="text" name="mport" id="mport" placeholder="mPort">
            <label for="mlogin">mLogin:</label>
            <input type="text" name="mlogin" id="mlogin" placeholder="mLogin">
            <label for="mpass">mPass:</label>
            <input type="text" name="mpass" id="mpass" placeholder="mPass">
            <label for="mprefix">TopControl:</label>
            <input type="text" name="topctrl" id="topctrl" placeholder="topCtrl">
            <label for="mprefix">TopInfo:</label>
            <input type="text" name="topinfo" id="topinfo" placeholder="topInfo">
            <br>
            <input type="submit" value="Submit">
        </form>
        <form action="/exit" method="POST">
            <input type="submit" value="Reboot">
        </form>
    </center>
</body>
</html>)rawliteral";

static bool _SP_started = false;
static byte _SP_status = 0;
PortalCfg portalCfg;

void SP_handleConnect()
{
  strcpy(portalCfg.SSID, _SP_server.arg("ssid").c_str());
  strcpy(portalCfg.pass, _SP_server.arg("pass").c_str());

  strcpy(portalCfg.mserver, _SP_server.arg("mserver").c_str());
  strcpy(portalCfg.mport, _SP_server.arg("mport").c_str());
  strcpy(portalCfg.mlogin, _SP_server.arg("mlogin").c_str());
  strcpy(portalCfg.mpass, _SP_server.arg("mpass").c_str());
  strcpy(portalCfg.topctrl, _SP_server.arg("topctrl").c_str());
  strcpy(portalCfg.topinfo, _SP_server.arg("topinfo").c_str());
  portalCfg.mode = WIFI_STA;
  _SP_status = 1;
}

// void SP_handleAP()
// {
//   portalCfg.mode = WIFI_AP;
//   _SP_status = 2;
// }

// void SP_handleLocal()
// {
//   portalCfg.mode = WIFI_STA;
//   _SP_status = 3;
// }

void SP_handleExit()
{
  _SP_status = 4;
}

void portalStart()
{
  WiFi.softAPdisconnect();
  WiFi.disconnect();

  IPAddress apIP(SP_AP_IP);
  IPAddress subnet(255, 255, 255, 0);

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, subnet);
  WiFi.softAP(SP_AP_NAME);
  _SP_dnsServer.start(53, "*", apIP);

  _SP_server.onNotFound([]()
                        { _SP_server.send(200, "text/html", SP_connect_page); });
  _SP_server.on("/connect", HTTP_POST, SP_handleConnect);
  // _SP_server.on("/ap", HTTP_POST, SP_handleAP);
  // _SP_server.on("/local", HTTP_POST, SP_handleLocal);
  _SP_server.on("/exit", HTTP_POST, SP_handleExit);
  _SP_server.begin();
  _SP_started = true;
  _SP_status = 0;
}

void portalStop()
{
  WiFi.softAPdisconnect();
  _SP_server.stop();
  _SP_dnsServer.stop();
  _SP_started = false;
}

bool portalTick()
{
  if (_SP_started)
  {
    _SP_dnsServer.processNextRequest();
    _SP_server.handleClient();

    yield();

    if (_SP_status)
    {
      portalStop();
      return 1;
    }
  }
  return 0;
}

void portalRun(uint32_t prd)
{
  uint32_t tmr = millis();
  portalStart();
  while (!portalTick())
  {
    if (millis() - tmr > prd)
    {
      _SP_status = 5;
      portalStop();
      break;
    }
    yield();
  }
}

byte portalStatus()
{
  return _SP_status;
}