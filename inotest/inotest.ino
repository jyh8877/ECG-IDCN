#include <Adafruit_MLX90614.h>

#include "XH.h"

#include <Arduino.h>
#include <string.h>
#include <time.h>

#include <ETH.h>
#include <WiFi.h>
#include <WiFiAP.h>
#include <WiFiClient.h>
#include <WiFiGeneric.h>
#include <WiFiMulti.h>
#include <WiFiScan.h>
#include <WiFiServer.h>
#include <WiFiSTA.h>
#include <WiFiType.h>
#include <WiFiUdp.h>

#include <Adafruit_GFX.h>    // Core graphics library
#include <Adafruit_ST7735.h> // Hardware-specific library for ST7735
#include <SPI.h>

#include <esp32-hal.h>
#include <PubSubClient.h>
#define ECG_PIN_0   34
#define TFT_CS      5
#define TFT_RST     15
#define TFT_DC      27
#define TFT_MOSI    23
#define TFT_MISO    19
#define TFT_SCLK    18
#define TFT_BLK     2
#define BUT_0       26

#define connectQT 2 //不连接为0 连接为1 mqtt为2

const char* MQTT_SERVER = "mqtts.heclouds.com";
const int MQTT_PORT = 1883;
#define PRODUCT_ID "kT99iMQIOI"
#define DEVICE_ID "esp32"
#define API_KEY "version=2018-10-31&res=products%2FkT99iMQIOI%2Fdevices%2Fesp32&et=1776242073&method=md5&sign=HhSuUM%2FUhkzoyyuN1zjR0w%3D%3D"
#define ONENET_TOPIC_PROP_SET "$sys/kT99iMQIOI/esp32/thing/property/set"
#define ONENET_TOPIC_PROP_POST "$sys/kT99iMQIOI/esp32/thing/property/post"
#define ONENET_POST_BODY_FORMAT "{\"id\":\"%u\",\"params\":%s}"

WiFiClient client;
PubSubClient MqttClient(client);

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
Adafruit_MLX90614 temp = Adafruit_MLX90614();
unsigned char UpdateLockFlag = 0;
unsigned char postMsgId = 0;
QueueHandle_t queueAD_TCP;
QueueHandle_t queueAD_DIS;
QueueHandle_t queueDIS_MQTT;
//QueueHandle_t queueTemp;//

/********************************/
static float xv[5],yv[5];
const float NUM[5] = {
    0.02837077015879f,0.01871766767648f, 0.04160766139016f,0.01871766767648f,
    0.02837077015879f
};
const float DEN[5] = {
    1,   -2.129832354491f,  1.967853808154f,  -0.8479686947171f,
     0.145731778114f
};
/*********************************/

const char *ssid = "iphone";//wifi热点名字
const char *password = "jyh888777";//wifi热点密码

const IPAddress serverIP(172,20,10,2); //欲访问的地址
uint16_t serverPort = 8888;         //服务器端口号

//const char* ntpServer = "cn.pool.ntp.org";
const char* ntpServer = "ntp.aliyun.com";//北京时间
const long  Offset_sec = 28800;
const int   daylightOffset_sec = 0;


static int i=0 , ecg_dis_cout = 0 ,ecg_bpm_dis = 0;
uint8_t wififlag = 0;//网络连接

uint8_t TEMPflag = 1 ,LCDflag = 1 ,NTPflag = 0 , ECG_disflag = 0;
float temp_ambient = 0,temp_object = 0;

void IRAM_ATTR isr() {
  digitalWrite(TFT_BLK,1^digitalRead(TFT_BLK));
}

void setup()
{
    Serial.begin(115200);
    initLCD(ST77XX_BLACK);
    taskbar_init();
    pinMode(ECG_PIN_0,INPUT);

    queueAD_TCP = xQueueCreate(100,sizeof(int));
    queueAD_DIS = xQueueCreate(80,sizeof(int));
    queueDIS_MQTT = xQueueCreate(80,sizeof(int));
//    queueTemp = xQueueCreate(100,sizeof(float));
    
    if(connectQT != 0)
    {
      WiFi.mode(WIFI_STA);
      WiFi.setSleep(false); //关闭STA模式下wifi休眠，提高响应速度
      WiFi.begin(ssid, password);
      while (WiFi.status() != WL_CONNECTED)//
        {
            delay(500);
            Serial.print(".");
        }
      Serial.println("Connected");
      Serial.print("IP Address:");
      Serial.println(WiFi.localIP());
      if(!AutoConfig())
      {
        SmartConfig();//配网
      }
    } else{
      wififlag = 1;
    }
 

    
    temp.begin();

    configTime(Offset_sec, daylightOffset_sec, ntpServer);
    if(connectQT != 0){
      while(!printLocalTime()){
        configTime(Offset_sec, daylightOffset_sec, ntpServer);
      }
    }
    if(connectQT == 2)
    {
        MqttClient.setServer(MQTT_SERVER, MQTT_PORT);
        delay(500);
        MqttClient.connect(DEVICE_ID, PRODUCT_ID, API_KEY);
        delay(500);
        reconnectMQTT();
    }
    
    WiFi.setTxPower(WIFI_POWER_5dBm);
    Serial.println(WiFi.getTxPower());
    xTaskCreatePinnedToCore(getADTask,"getADTask",2048,NULL,1,NULL,0);//最后的0代表cpu核心1，一共双核
    xTaskCreatePinnedToCore(LCDTask,"LCDTask",2048,NULL,2,NULL,1);
    if(connectQT == 1)
    {
      xTaskCreatePinnedToCore(socketTask,"socketTask",2560,NULL,1,NULL,0);//网络连接，单片机单独运行
    }
    else if(connectQT == 2)
    {
      
       xTaskCreatePinnedToCore(mqttTask,"mqttTask",4096,NULL,1,NULL,0);//mqtt连接，单片机单独运行
      
    }

    attachInterrupt(BUT_0,isr,FALLING);
    
}

void loop()
{
//    delay(1000);
}

////***********************************task1
void getADTask(void *parameter){
  int analog_value = 0;
  int ad_time = 0;
  int ad_cout = 0 ;
  TickType_t lasttick = xTaskGetTickCount();
  for(;;){
    while(wififlag){
      ad_time = micros();
      vTaskDelayUntil(&lasttick, 5);//5ms执行一次
      analog_value = analogRead(ECG_PIN_0);
      analog_value = filter_low_40(analog_value)/35;//40Hz低通滤波
      xQueueSend(queueAD_TCP,&analog_value,0);
      if(++ad_cout>=5&&ECG_disflag == 0){
        ad_cout = 0;
        xQueueSend(queueAD_DIS,&analog_value,0);
        if(++ecg_dis_cout>=80){
          ecg_dis_cout = 0;
          ECG_disflag = 1;
        }
      }
      ad_time = micros() - ad_time;
      // Serial.printf("get ad time:%d\r\n",ad_time);
    }
    vTaskDelay(20);
    lasttick = xTaskGetTickCount();
  }
  vTaskDelete(NULL);
}


////***********************************task2
void socketTask(void *parameter){
  int AD_value = 0;
  int tcp_time = 0;
  int AD_cout = 0;
  char str_out[250] = {0};
  char str_in[10] = {0};
  for(;;){
    if (client.connect(serverIP, serverPort)) //尝试访问目标地址
    {
//        Serial.println("访问成功");
        TickType_t lasttick = xTaskGetTickCount();
        while (client.connected() || client.available()) //如果已连接或有收到的未读取的数据
        {
            tcp_time = micros();
            vTaskDelayUntil(&lasttick, 5);//100ms发送一次数据
            wififlag = 1;
            if(AD_cout<20){
              xQueueReceive(queueAD_TCP,&AD_value,0);
              sprintf(str_in,"AD:%d:",AD_value);
              strcat(str_out,str_in);
              memset(str_in,0,sizeof(str_in));
              AD_cout++;
            }
            else if(AD_cout == 20){
              client.printf("%sTEMP:%0.2f",str_out,temp_object);
              memset(str_out,0,sizeof(str_out)); 
              AD_cout = 0;
            }
            tcp_time = micros()-tcp_time;
            Serial.printf("get tcp time:%d\r\n",tcp_time);
        }
        wififlag = 0;
        client.stop(); //关闭客户端
    }
    else
    {
//        Serial.println("访问失败");
        client.stop(); //关闭客户端
    }
    vTaskDelay(20);
  }
  vTaskDelete(NULL);
}
void reconnectMQTT() {
  while (!MqttClient.connected()) {
    // String clientId = "ESP32-" + DEVICE_ID;
    if (MqttClient.connect(DEVICE_ID, PRODUCT_ID, API_KEY)) {
      Serial.println("MQTT Connected");
    } else {
      Serial.print("Failed, rc=");
      Serial.print(MqttClient.state());
      delay(5000);
    }
  }
}
void Send(unsigned char val1, unsigned char val2, unsigned char val3, unsigned char val4, unsigned char val5,
          unsigned char val6, unsigned char val7, unsigned char val8, unsigned char val9, unsigned char val10)
{
    if (!MqttClient.connected()) {
        Serial.println("MQTT not connected, skipping send");
        return;
    }
    
    // 先拼接出json字符串
    char param[82];
    char jsonBuf[178];
    
    snprintf(param, sizeof(param), "{\"Heart\":{\"value\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d]}}", 
             val1, val2, val3, val4, val5, val6, val7, val8, val9, val10);
    
    postMsgId += 1;
    snprintf(jsonBuf, sizeof(jsonBuf), ONENET_POST_BODY_FORMAT, postMsgId, param);
    
    // 发布消息
    if (MqttClient.publish(ONENET_TOPIC_PROP_POST, jsonBuf)) {
        Serial.print("Post message to cloud: ");
        Serial.println(jsonBuf);
    } else {
        Serial.println("Publish message to cloud failed!");
    }
}
void mqttTask(void *parameter)
{
    TickType_t lasttick = xTaskGetTickCount();
    unsigned char AdCount = 0;
    unsigned int AdBuf[10];
    unsigned int Ad_value = 0;
    unsigned long lastReconnectAttempt = 0;
    
    // 等待WiFi连接
    while (WiFi.status() != WL_CONNECTED) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    
    wififlag = 1;
    
    while(1)
    {
      // vTaskDelayUntil(&syncTick, 100);
        // 检查MQTT连接状态
        if (!MqttClient.connected()) {
            unsigned long now = millis();
            if (now - lastReconnectAttempt > 5000) {  // 每5秒尝试重连一次
                lastReconnectAttempt = now;
                Serial.println("MQTT disconnected, attempting to reconnect...");
                reconnectMQTT();
            }
        } else {
            // MQTT已连接，处理数据
            // vTaskDelayUntil(&lasttick, 15);
            while(AdCount<10)
            {
              if (xQueueReceive(queueDIS_MQTT, &Ad_value, pdMS_TO_TICKS(10)) == pdTRUE) {
                AdBuf[AdCount] = Ad_value;
                AdCount++;
              }
            }
              AdCount = 0;
              Send(AdBuf[0], AdBuf[1], AdBuf[2], AdBuf[3], AdBuf[4], 
                         AdBuf[5], AdBuf[6], AdBuf[7], AdBuf[8], AdBuf[9]);
        }
        MqttClient.loop();
        // vTaskDelay(10);
    }
    
    vTaskDelete(NULL);
}
////******************************************task3
void LCDTask(void *parameter){
    char TEMPDIS[10] = {0} , BPMDIS[10] = {0};
    uint8_t rssi_X = 1;
    int AD_DIS = 2000, AD_LAST = 2000;
    int lcd_time = 0;
    uint8_t ntp_cout = 0,temp_cout = 0;
    uint8_t clearflag = 0;
    for(;;){
      TickType_t lasttick = xTaskGetTickCount();
      while (LCDflag) //尝试访问目标地址
      {
          lcd_time = micros();
          vTaskDelayUntil(&lasttick, 100);
          if(++ntp_cout>=10 && connectQT == 1)
          {
            ntp_cout = 0;
            printLocalTime();
          }
          if(wififlag == 1)
          {
            /************************************/
            if(ECG_disflag == 1){
              xQueueReceive(queueAD_DIS,&AD_DIS,0);
              if(AD_DIS>2800) AD_DIS = 2800;
              else if (AD_DIS<1500) AD_DIS = 1500;
              if(AD_DIS<=2800&&AD_DIS>=1500){
                if(rssi_X == 1){
                  tft.fillRect(0,24,160,80,ST77XX_BLACK);
                }
                
//                tft.fillRect(rssi_X,24,2,80,ST77XX_BLACK);
                int y2 = 24+(int)((2800-AD_DIS)/20);
                tft.drawLine(rssi_X,24+(int)((2800-AD_LAST)/20),rssi_X+2,y2,ST77XX_WHITE);
                y2 = 80 - y2;
                xQueueSend(queueDIS_MQTT,&y2,0);
                AD_LAST = AD_DIS;
              }
              rssi_X+=2;
//              if (rssi_X>=158){
//                rssi_X = 0;
//              }
              if(++ecg_dis_cout >= 80 || rssi_X>=158){
                ecg_dis_cout = 0;
                ECG_disflag = 0;
                rssi_X = 1;
              }
            }
            /*************************************/         
            if(clearflag == 1)
            {
              tft.drawRGBBitmap(160-24,0,gImage_NET,24,24);
            }
            clearflag = 0;
          }
          else if(wififlag == 0&&clearflag == 0)
          {
            clearflag = 1;
            tft.fillRect(0,24,tft.width(),8*11,ST77XX_BLACK);
            tft.drawRGBBitmap(160-24,0,gImage_unNET,24,24);
            printText(0,24,"Try to access the server...",ST77XX_WHITE,1);
          }


          /**************************************/
          if(temp_cout++>=4)
          {
            if(temp_object >= 30&&temp_object <= 50){
              sprintf(TEMPDIS,"%4.1f",temp_object);
              tft.fillRect(85,4,12*4,16,ST77XX_BLACK);
              printText(85,4,TEMPDIS,ST77XX_WHITE,2);
            }
            temp_cout = 0;
          }

          if (client.available()) //如果有数据可读取
            {
                String line = client.readStringUntil('\n'); //读取数据到换行符
                ecg_bpm_dis = line.toInt();
                Serial.print("读取到数据：");
                Serial.println(ecg_bpm_dis);
//                client.write(line.c_str()); //将收到的数据回发
            }
          if(ecg_bpm_dis != 0)
          { 
            sprintf(BPMDIS,"%d",ecg_bpm_dis);
            tft.fillRect(24,4,12*3,16,ST77XX_BLACK);
            printText(24,4,BPMDIS,ST77XX_WHITE,2);
            ecg_bpm_dis = 0;
          }
          
          /**************************************/
          lcd_time = micros()-lcd_time;
          // Serial.printf("get lcd time:%d\r\n",lcd_time);
      }
    vTaskDelay(5);
    }
   vTaskDelete(NULL);
}

////*************************************task4
void tempTask(void *parameter)
{
  int temp_time = 0;
  int temp_cout = 0;
  float temp_object_c = 0;
  for(;;){
    TickType_t lasttick = xTaskGetTickCount();
    while(TEMPflag){
      temp_time = micros();
      vTaskDelayUntil(&lasttick, 10);
      if(temp_cout++>=10)
      {
        temp_cout = 0;
        temp_ambient = temp.readAmbientTempC();
        temp_object_c = temp.readObjectTempC();
        if(temp_object_c>=32){
          temp_object = 0.0012*pow(temp_object_c,3)-0.1313*pow(temp_object_c,2)+4.8794*temp_object_c-24.314;
        }
        else temp_object = 0;
        
        
        Serial.printf("Ambient:%0.2f\r\nObject:%0.2f\r\n",temp_ambient,temp_object);
        
      }
      temp_time = micros()-temp_time;
      Serial.printf("get temp time:%d\r\n",temp_time);
    }
    vTaskDelay(5);
  }
  vTaskDelete(NULL);
}


void initLCD(uint16_t color){
    tft.initR(INITR_BLACKTAB);
    tft.setRotation(3);
    tft.setSPISpeed(40000000);
    tft.fillScreen(color);
    
}
void printText(uint8_t x,uint8_t y, char *txt,uint16_t color,uint8_t txtsize){
  tft.setCursor(x, y);
  tft.setTextColor(color);
  tft.setTextSize(txtsize);// 1 is default 6x8, 2 is 12x16, 3 is 18x24
  tft.print(txt);
}

void SmartConfig()
{
    WiFi.mode(WIFI_STA);
    Serial.println("WIFI Wait for Smartconfig");
    WiFi.beginSmartConfig();
    while (1)
    {
        Serial.print(".");
        if (WiFi.smartConfigDone())
        {
        Serial.println("WIFI SmartConfig Success");
        Serial.printf("SSID:%s", WiFi.SSID().c_str());
        Serial.printf(", PSW:%s\r\n", WiFi.psk().c_str());
        Serial.print("LocalIP:");
        Serial.print(WiFi.localIP());
        Serial.print(" ,GateIP:");
        Serial.println(WiFi.gatewayIP());
        WiFi.setAutoReconnect(true);  // 设置自动连接
        break;
        }
        delay(1000);
    }
}

bool AutoConfig()
{
    WiFi.begin();
    for (int i = 0; i < 5; i++)
    {
        int wstatus = WiFi.status();
        if (wstatus == WL_CONNECTED)
        {
          Serial.printf("SSID:%s", WiFi.SSID().c_str());
          Serial.printf(", PSW:%s\r\n", WiFi.psk().c_str());
          Serial.print("LocalIP:");
          Serial.print(WiFi.localIP());
          Serial.print(" ,GateIP:");
          Serial.println(WiFi.gatewayIP());
          return true;
        }
        else
        {
          Serial.print("WIFI AutoConfig Waiting......");
          Serial.println(wstatus);
          delay(1000);
        }
    }
    return false;
}


bool printLocalTime()//获取网络时间
{
  struct tm timeinfo;
  char NTPdate[20]={0},NTPtime[3]={0};
  static uint8_t lasttime[3]={0};
  char *NTPweek[7]={"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
  if(!getLocalTime(&timeinfo)){
    Serial.println("Failed to obtain time");
    return 0;
  }
  else{
//    Serial.printf("week %s",timeinfo.tm_wday);
    if(NTPflag == 0){
      sprintf(NTPdate,"%04d-%02d-%02d",timeinfo.tm_year+1900,timeinfo.tm_mon+1,timeinfo.tm_mday);
      printText(0,128-8, NTPdate,ST77XX_WHITE,1);//日期
      printText(0,128-16, NTPweek[timeinfo.tm_wday],ST77XX_WHITE,1);//星期
      printText(160-96,128-16,"  :  :  ",ST77XX_WHITE,2);
      NTPflag = 1;
    }
//    sprintf(NTPtime,"%02d:%02d:%02d",timeinfo.tm_hour,timeinfo.tm_min,timeinfo.tm_sec);
    if(lasttime[0]!=timeinfo.tm_hour){
      lasttime[0] = timeinfo.tm_hour;
      sprintf(NTPtime,"%02d",timeinfo.tm_hour);
      tft.fillRect(160-8*12,128-16,2*12,16,ST77XX_BLACK);
      printText(160-8*12,128-16,NTPtime,ST77XX_WHITE,2);
    }
    if(lasttime[1]!=timeinfo.tm_min){
      lasttime[1] = timeinfo.tm_min;
      sprintf(NTPtime,"%02d",timeinfo.tm_min);
      tft.fillRect(160-5*12,128-16,2*12,16,ST77XX_BLACK);
      printText(160-5*12,128-16,NTPtime,ST77XX_WHITE,2);
    }
    if(lasttime[2]!=timeinfo.tm_sec/10){
      lasttime[2] = timeinfo.tm_sec/10;
      sprintf(NTPtime,"%01d",timeinfo.tm_sec/10);
      tft.fillRect(160-2*12,128-16,12,16,ST77XX_BLACK);
      printText(160-2*12,128-16,NTPtime,ST77XX_WHITE,2);
    }
    sprintf(NTPtime,"%01d",timeinfo.tm_sec%10);
    tft.fillRect(160-12,128-16,12,16,ST77XX_BLACK);
    printText(160-12,128-16,NTPtime,ST77XX_WHITE,2);

//    printText(160-96,128-16, NTPtime,ST77XX_WHITE,2);//1 is default 6x8, 2 is 12x16, 3 is 18x24
  //  Serial.println(&timeinfo, "%A, %Y-%m-%d %H:%M:%S");
  return 1;
  }
}

void taskbar_init()
{
  tft.drawRGBBitmap(0,0,gImage_ECG,24,24);
  tft.drawRGBBitmap(24*2+12,0,gImage_TEMP,24,24);
  pinMode(TFT_BLK,OUTPUT);
  digitalWrite(TFT_BLK,HIGH);
  pinMode(BUT_0,INPUT_PULLUP);
}

float filter_low_40(float inputValue) //40Hz低通滤波
{
    xv[0] = xv[1];xv[1] = xv[2];xv[2] = xv[3];xv[3] = xv[4];xv[4] = inputValue/0.02837077f;
    yv[0] = yv[1];yv[1] = yv[2];yv[2] = yv[3];yv[3] = yv[4];
    yv[4] = NUM[0]*xv[4]+NUM[1]*xv[3]+NUM[2]*xv[2]+NUM[3]*xv[1]+NUM[4]*xv[0]-DEN[1]*yv[3]-DEN[2]*yv[2]-DEN[3]*yv[1]-DEN[4]*yv[0];
    return yv[4];
}
