#include <WiFi.h>
#include <WebServer.h>
#include <EEPROM.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#define EEPROM_SIZE 512

//Обьявление глобальных параметров
const int LED_ALERT = 12;
const int BTN_RESET = 14;
const int RELAY_1 = 25;
const int RELAY_2 = 26;
const int COM_SPEED = 115200;
const int WEB_SERVER_PORT = 80;
const char* ssid = "light_signal";

//IP конфигурация для сервера 
IPAddress local_ip(192, 168, 1, 1); // IP адрес
IPAddress gateway(192, 168, 1, 1); // IP шлюз
IPAddress subnet(255, 255, 255, 0); // IP маска подсети

WebServer server(WEB_SERVER_PORT);

TaskHandle_t WebServerWorker;
TaskHandle_t StatusLEDWorker;
TaskHandle_t ResetBtnListener;
TaskHandle_t MainWorker;

struct Configuration {
    byte configured;
    long check_interval;
    char ssid[32];
    char password[32];
    char address[120]; 
    long blink_interval;
};

struct CurrentStatus {
    bool stage;
    bool prod;
    bool configured_and_connected;
    long last_check;
};

CurrentStatus CURRENT;
Configuration CONF;

void setup() {
    
    //Initialize serial
    Serial.begin(COM_SPEED);
    Serial.println("Starting...");
    Serial.println("COM port initialized");

    //Initialize pin modes
    pinMode(LED_ALERT, OUTPUT);
    pinMode(RELAY_1, OUTPUT);
    pinMode(RELAY_2, OUTPUT);
    pinMode(BTN_RESET, INPUT);
    digitalWrite(LED_ALERT, HIGH);
    digitalWrite(RELAY_1, HIGH);
    digitalWrite(RELAY_2, HIGH);
    Serial.println("Ports initialized");
    
    //Init task for LED 
    xTaskCreatePinnedToCore(StatusLEDWorkerCode, "StatusLEDWorker", 10000, NULL, 1, &StatusLEDWorker,1);

    //Startup 
    EEPROM.begin(EEPROM_SIZE);
    Serial.println("Start reading configuration");
    CONF = readConfiguration();
    if(CONF.configured == 0x01) {
        Serial.println("Configuration from EEPROM loaded");
        startWiFiClient(CONF);
        digitalWrite(RELAY_1, LOW);
        digitalWrite(RELAY_2, LOW);
        CURRENT.configured_and_connected = true;
    } else {
        Serial.println("Configuration not found startup as AP");
        startWiFiAP();  
    }
    
    //Initialize web server
    server.on("/", handlerPage); 
    server.on("/action", handlerSave); 
    Serial.println("Added handler to web server: '/'");
    server.on("/reboot", handlerReboot); 
    Serial.println("Added handler to web server: '/reboot'");
    server.onNotFound(handlerPage);
    Serial.println("Added handler to web server for not found");
    server.begin();
    Serial.println("Web server started on 0.0.0.0:80");
    
    //Start background workers 
    xTaskCreatePinnedToCore(WebServerWorkerCode, "WebServerWorker", 10000, NULL, 1, &WebServerWorker, 0);    
    xTaskCreatePinnedToCore(ResetBtnListenerCode, "ResetBtnListener", 10000,  NULL, 1, &ResetBtnListener, 0);

    

}
void handlerSave() {  
      Configuration conf = readConfiguration();
      Serial.println("Save configuration command");
      for (uint8_t i = 0; i < server.args(); i++) {
          if (server.argName(i) == "address") {
                server.arg(i).toCharArray(conf.address, 120);
                Serial.println("Address: " + server.arg(i));
          } else if (server.argName(i) == "password") {
                server.arg(i).toCharArray(conf.password, 32);
                Serial.println("Password: " + server.arg(i));
          } else if (server.argName(i) == "ssid") {
                server.arg(i).toCharArray(conf.ssid, 32);
                Serial.println("SSID: " + server.arg(i));
          } else if (server.argName(i) == "check_interval") {
                conf.check_interval = String(server.arg(i)).toInt();
                Serial.println("Check interval: " + String(server.arg(i)) + "s");           
          } else if (server.argName(i) == "blink_interval") {
                conf.blink_interval = String(server.arg(i)).toInt();
                Serial.println("Blink interval: " + String(server.arg(i)) + "ms");           
          } 
      }
      if(server.args() > 0) {
          Serial.println("Try save configuration");
          writeConfiguration(conf);
      }
      server.sendHeader("Location", "/?message=Конфигурация сохранена",true); //Redirect to our html web page 
      server.send(302, "text/plane",""); 
}

void handlerReboot() {
    server.sendHeader("Location", "/?message=Отправлено в перезагрузку!",true); //Redirect to our html web page 
    server.send(302, "text/plane",""); 
    reboot();
}  

void handlerPage() {  
      String state = "";   
      Configuration conf = readConfiguration();
      if(server.args() > 0) {
          Serial.println("Command update configuration received");
        }
      for (uint8_t i = 0; i < server.args(); i++) {
          if (server.argName(i) == "message") {
                state = String(server.arg(i));
          } 
      }
      server.send(200, "text/html", htmlPage(conf, state));
  }

void loop() {
     if(!CURRENT.configured_and_connected) {
          Serial.println("loop() -> Server not configured");
          BlinkDelay(1,1000);
          return;
     }
     if((WiFi.status() != WL_CONNECTED)) {
          Serial.println("loop() -> WIFI not connected");
          startWiFiClient(CONF);
     }
     delay(CONF.check_interval * 1000);
     HTTPClient http;
     http.begin(CONF.address);
     Serial.println("loop() -> GET " + String(CONF.address));
     int httpCode = http.GET();
     if (httpCode >= 100 && httpCode < 400) {
        String body = http.getString();
        char copy[50];
        body.toCharArray(copy, 50);
        Serial.println("loop() -> Response " + body);
        StaticJsonDocument<50> json;
        DeserializationError error = deserializeJson(json, copy); 
        if(error) {
            CURRENT.stage = false;
            CURRENT.prod = false;
            BlinkDelay(6,1000);
            CURRENT.stage=true;
            CURRENT.prod=true;
            return;
        } 
        if(json["stage"] == 1) {
              CURRENT.stage = true;
          }  else {
              CURRENT.stage = false;
          }   
        if(json["prod"] == 1) {
              CURRENT.prod = true;
          }  else {
              CURRENT.prod = false;
          }
          CURRENT.last_check = millis();
      } else {
        Serial.println("Error on HTTP request");
        CURRENT.stage = false;
        CURRENT.prod = false;
        BlinkDelay(6,1000);
        CURRENT.stage=true;
        CURRENT.prod=true;
        return;
      }
      http.end(); //Free the resources
  }


void startWiFiAP() {
    WiFi.softAP(ssid);
    Serial.println("Started AP with SSID = " + String(ssid));
    WiFi.softAPConfig(local_ip, gateway, subnet);
}

bool isWiFiConnected() {
    return WiFi.status() == WL_CONNECTED;
  }
void startWiFiClient(Configuration conf) {
    WiFi.begin(conf.ssid, conf.password);
    Serial.println("Connecting to WiFi " + String(conf.ssid) + " with password " + String(conf.password));
    while (WiFi.status() != WL_CONNECTED) { 
        Serial.print(".");
        BlinkDelay(2,100);
        delay(200);
    }
    Serial.println("");
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());
}

void reboot() {
    BlinkDelay(8, 100);
    ESP.restart();
  }


void resetConfiguration() {
      EEPROM.put(0, Configuration{0x00, 0, "", "", ""});
      EEPROM.commit();
      BlinkDelay(5, 100);
  }
Configuration readConfiguration() { 
       Configuration conf;
       EEPROM.get(0, conf);
       return conf;
  }
void writeConfiguration(Configuration conf) {
       conf.configured = 0x01;
       EEPROM.put(0, conf);
       EEPROM.commit();
       BlinkDelay(5, 100);
  }


void BlinkDelay(int Count, int Delay) {
    digitalWrite(LED_ALERT, LOW);
    for(int i = 0; i < Count; i++) {
      digitalWrite(LED_ALERT, HIGH);
      delay(Delay);
      digitalWrite(LED_ALERT, LOW);
      delay(Delay);   
    } 
}



/*
================================================================
================== Tasks runned on CORE 1   ====================
================================================================
*/
void StatusLEDWorkerCode( void * pvParameters ){
  Serial.println("Blink status worker started");
  for(;;) {
    if( CURRENT.last_check > 0 ) {
        break;
    }
    delay(100);
  }
  int prod_changed = millis();
  int stage_changed = millis();
  bool stage_blink = false;
  bool prod_blink = false;
  for(;;){
    int stage_interval = millis() - stage_changed;
    int prod_interval = millis() - prod_changed;
    
    if( !CURRENT.stage ) {
        if( stage_interval > CONF.blink_interval ) {
          stage_changed = millis();
          stage_blink = !stage_blink;
          digitalWrite(RELAY_1, stage_blink);
        }  
    } else {
      digitalWrite(RELAY_1, LOW);  
    } 
    
    if( !CURRENT.prod ) {
        if( prod_interval > CONF.blink_interval ) {
          prod_changed = millis();
          prod_blink = !prod_blink;
          digitalWrite(RELAY_2, prod_blink);
        }
    } else {
      digitalWrite(RELAY_2, LOW);  
    } 
  }
}



/*
================================================================
================== Tasks runned on CORE 0   ====================
================================================================
*/
void WebServerWorkerCode( void * pvParameters ){
  Serial.println("Web server worker started");
  delay(1000);
  for(;;){
    server.handleClient(); 
    delay(10);  
  } 
}
void ResetBtnListenerCode( void * pvParameters ){
  Serial.println("Reset btn listener started");
  delay(500);
  long resetTimeout = millis();
  for(;;){
    if(digitalRead(BTN_RESET) == HIGH ) {
        Serial.print("R");
        if((millis() - resetTimeout) > 3000 ) {
          Serial.println("Reset button clicked more then 3s, reseting...");
          resetConfiguration();
          reboot();  
        }
      } else {
        resetTimeout = millis();  
      }
      delay(50);
  }
}




String htmlPage(Configuration conf, String state){
  String configured = "<span style='color: red'>Не настроена</span>";
  String ptr = R""""(<!DOCTYPE html>
<html>
    <head>
      <meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
      <meta charset="UTF-8">
      <title>Управление световой сигнализацией</title>
      <style>
        html { 
          font-family: Helvetica;  
          text-align: center;
     
        }
        body{
          margin-top: 50px;
        } 
        h1 {
          color: #444444; 
        } 
        h3 {
          color: #444444; 
        }
        .button {
          display: block;
          background-color: #3458db;
          border: none;
          color: white;
          padding: 15px 15px;
      margin: 0px 0px;
      width: 90%;
          text-decoration: none;
          font-size: 18px;
          cursor: pointer;
          border-radius: 4px;
        }
        input {
          display: block;
          border: 1px solid gray;
          padding: 15px 15px;
      margin: 0px 0px;
      width: 90%;
          text-decoration: none;
          font-size: 18px;
          cursor: pointer;
          border-radius: 4px;
        }
        .button-on {
          background-color: #3498db;
        }
        .button-on:active {
          background-color: #2980b9;
        }
        .button-off {
          background-color: #34495e;
        }
        .button-off:active {
          background-color: #2c3e50;
        }
    table {
      text-align: left; 
      width: 100%;
    }
    table tr td {
      padding: 2px;
    }
    .main {
      width: 90%;
      display: block;
      margin: 0 auto;
      height: auto; 
      padding: 15px;
      border: 1px solid black;
      background: #F0F0F0;
      border-radius: 5px;
    }
        </style>
    </head>
    <body>
  <div class="main">
      <h1>Настройка сигнализации</h1>
      <h3>Текущее состояние: {{CONFIG_STATUS}}</h3>
      {{STATUS}}
    <form method="GET" action="/action">
    <table >
       <tr><td colspan="2"><h3>Настройка WIFI подключения</td></tr> 
       <tr>
           <td>SSID<br><small>(Имя точки доступа)</small></td>
         <td><input name="ssid" value="{{SSID}}" placeholder="wifi_office"  required /></td>
      </tr>
       <tr>
           <td>Пароль<br><small>(Пароль точки доступа)</small></td>
         <td><input name="password"  value="{{PASSWORD}}" placeholder="12345678"  required /></td>
      </tr> 
      
       <tr><td colspan="2"><h3>Настройка опросника</td></tr>  
       <tr>
           <td>Адрес<br><small>(Адрес опроса, где брать состояние)</small></td>
         <td><input name="address" value="{{ADDRESS}}" placeholder="http://mon.loc/monitoring" pattern="(http|https):\/\/.*" required /></td>
      </tr>
       <tr>
           <td>Интервал опроса<br><small>(Частота опроса в секундах)</small></td>
         <td><input name="check_interval" value="{{CHECK_INTERVAL}}" placeholder="1" pattern="[0-9]+" required /></td>
      </tr>
      <tr>
       <tr>
           <td>Интервал мигания<br><small>(Частота мигания в миллисекундах)</small></td>
         <td><input name="blink_interval" value="{{BLINK_INTERVAL}}" placeholder="1" pattern="[0-9]+" required /></td>
      </tr>
      <tr>
           <td></td>
         <td><button class="button" type="submit" action="save">Сохранить</button></td>
      </tr>
      <tr>
           <td></td>
         <td><a href="/reboot">Перезагрузка</button></td>
      </tr>
      
  </form>
  </div>
  </body>
</html>)"""";

    if(conf.configured) {
      configured = "<span style='color: green'>Настроена</span>";
    }
     if(state != "") {
      ptr.replace("{{CONFIG_STATUS}}", state);
     } else {
        ptr.replace("{{CONFIG_STATUS}}", configured); 
     }
     ptr.replace("{{ADDRESS}}", conf.address);
     ptr.replace("{{CHECK_INTERVAL}}", String(conf.check_interval));
     ptr.replace("{{BLINK_INTERVAL}}", String(conf.blink_interval));
     ptr.replace("{{SSID}}", String(conf.ssid));
     ptr.replace("{{PASSWORD}}", String(conf.password));

    String status = "";
    if(CURRENT.stage) {
       status += "Stage: <span style='color: green; font-weight: bold'>работает</span><br>";
    } else {
       status += "Stage: <span style='color: red; font-weight: bold'>не работает</span><br>";
    }
    if(CURRENT.prod) {
       status += "Production: <span style='color: green; font-weight: bold'>работает</span><br>";
    } else {
       status += "Production: <span style='color: red; font-weight: bold'>не работает</span><br>";
    }
    status += "Последнее обновление: " + String(millis() - CURRENT.last_check) + "ms назад"; 
    ptr.replace("{{STATUS}}", status);
  return ptr;
}
