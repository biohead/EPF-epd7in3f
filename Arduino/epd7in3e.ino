#include <Arduino.h>
#include <SPI.h>
#include <HTTPClient.h>
#include "epd7in3e.h"
#include "FS.h"
#include "LittleFS.h"
#include <ArduinoJson.h>
#include "SimpleWiFiManager.h"

/* PIN LAYOUT

DRIVER BOARD  <>  FireBeetle ESP32-C6

BUSY          <>  18
RST           <>  14
DC            <>  8
CS            <>  1
SCLK          <>  23
DIN           <>  22
GND           <>  GND
VCC           <>  3V3
SETTING       <>  19
*/

const char *const AP_NAME = "AP-NAME";
// const char *const CONFIG_FILE = "/config.json";
// const char *const IMAGE_FILE = "/image_data.c";
constexpr uint16_t HTTP_TIMEOUT = 50000;
constexpr uint16_t RETRY_DELAY = 10000;
constexpr uint8_t MAX_RETRIES = 5;
constexpr size_t BUFFER_SIZE = 131072;
constexpr uint16_t SLEEP_INTERVAL = 300;

// GPIO
constexpr uint8_t CONFIG_PIN = 19;
constexpr uint16_t BUTTON_DEBOUNCE = 100;

class EpaperManager
{
private:
  SimpleWiFiManager wifiManager;
  Epd epd;
  String imageUrl = "";

  bool initFilesystem()
  {
    if (!LittleFS.begin(true))
    {
      Serial.println(F("LittleFS Mount Failed"));
      return false;
    }
    return true;
  }

  bool downloadImage()
  {
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT);

    if (!http.begin(imageUrl))
    {
      Serial.println(F("Failed to initialize HTTP connection"));
      return false;
    }

    for (uint8_t i = 0; i < MAX_RETRIES; i++)
    {
      int httpCode = http.GET();

      if (httpCode == HTTP_CODE_OK)
      {
        bool result = processImageData(&http);
        http.end();
        return result;
      }
      else if (httpCode == HTTP_CODE_ACCEPTED)
      {
        Serial.println(F("Server processing, waiting..."));
        delay(RETRY_DELAY);
      }
      else
      {
        Serial.printf("HTTP GET failed: %s\n", http.errorToString(httpCode).c_str());
        http.end();
        return false;
      }
    }

    http.end();
    return false;
  }

  bool isDelimiter(char c)
  {
    return c == ',' || c == '\n' || c == '\r' || c == '\0';
  }

  bool processImageData(HTTPClient *http)
  {
    WiFiClient *stream = http->getStreamPtr();
    int contentLength = http->getSize();

    if (contentLength <= 0)
    {
      Serial.println(F("Invalid content length"));
      return false;
    }

    Serial.printf("Content-Length: %d bytes\n", contentLength);
    Serial.println(F("Starting direct image processing..."));

    epd.SendCommand(0x10);

    uint8_t *buffer = (uint8_t *)malloc(BUFFER_SIZE);
    if (buffer == NULL)
    {
      Serial.println("Buffer allocation failed");
      return false;
    }
    String hexBuffer;
    int totalBytesProcessed = 0;

    while (contentLength > 0 && http->connected())
    {
      int bytesToRead = min(contentLength, (int)sizeof(buffer));
      int bytesRead = stream->readBytes(buffer, bytesToRead);

      if (bytesRead > 0)
      {
        for (int i = 0; i < bytesRead; i++)
        {
          char c = (char)buffer[i];
          if (isDelimiter(c))
          {
            if (!hexBuffer.isEmpty())
            {
              uint8_t byteValue = (uint8_t)strtol(hexBuffer.c_str(), nullptr, 16);
              epd.SendData(byteValue);
              hexBuffer.clear();
            }
          }
          else
          {
            hexBuffer += c;
          }
        }

        totalBytesProcessed += bytesRead;
        contentLength -= bytesRead;
      }
    }

    if (!hexBuffer.isEmpty())
    {
      uint8_t byteValue = (uint8_t)strtol(hexBuffer.c_str(), nullptr, 16);
      epd.SendData(byteValue);
    }
    free(buffer);
    Serial.println(F("Showing image"));
    epd.TurnOnDisplay();
    epd.Sleep();

    return true;
  }

  void hibernate()
  {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(1000);
    esp_sleep_enable_timer_wakeup(SLEEP_INTERVAL * 1000000ULL);
    esp_deep_sleep_start();
  }

  bool shouldEnterConfigMode()
  {
    if (digitalRead(CONFIG_PIN) == LOW)
    {
      delay(BUTTON_DEBOUNCE);
      return digitalRead(CONFIG_PIN) == LOW;
    }
    return false;
  }

public:
  bool begin()
  {
    Serial.begin(115200);
    pinMode(CONFIG_PIN, INPUT_PULLUP);

    if (epd.Init() != 0)
    {
      Serial.println(F("e-Paper init failed"));
      return false;
    }
    Serial.println(F("e-Paper initialized successfully"));

    if (!initFilesystem())
    {
      return false;
    }
    Serial.println(F("Filesystem initialized"));
    wifiManager.begin();
    if (shouldEnterConfigMode())
    {
      Serial.println(F("Entering config mode..."));
      epd.Clear(EPD_7IN3E_WHITE);
      epd.Sleep();
      wifiManager.begin();
      while (!wifiManager.handleConfig())
      {
        delay(10);
      }
      Serial.println(F("Config mode completed"));
    }

    Serial.println(F("Checking WiFi configuration"));

    if (wifiManager.isConfigured())
    {
      imageUrl = wifiManager.getParam1();
      Serial.printf("WiFi Configured. Image URL: %s\n", imageUrl.c_str());

      // Additional WiFi connection debugging
      // Serial.println(F("Waiting for WiFi connection..."));
      // int timeout = 20;
      // while (WiFi.status() != WL_CONNECTED && timeout > 0)
      // {
      //   Serial.print(".");
      //   delay(1000);
      //   timeout--;
      // }

      // if (WiFi.status() == WL_CONNECTED)
      // {
      //   Serial.println(F("\nWiFi Connected successfully"));
      //   Serial.print(F("IP Address: "));
      //   Serial.println(WiFi.localIP());
      //   return true;
      // }
      // else
      // {
      //   Serial.println(F("\nFailed to connect to WiFi"));
      //   return false;
      // }
      return true;
    }

    Serial.println(F("No valid WiFi configuration found - main"));
    return false;
  }

  void update()
  {
    Serial.println(F("Update method called"));

    if (WiFi.status() == WL_CONNECTED)
    {
      Serial.println(F("WiFi Connected. Downloading image"));
      if (downloadImage())
      {
        Serial.println(F("Image download successful"));
      }
      else
      {
        Serial.println(F("Image download failed"));
      }
    }
    else
    {
      Serial.println(F("WiFi not connected. Cannot download image"));
    }

    Serial.println(F("Entering sleep mode"));
    hibernate();
  }
};

// Global instance
EpaperManager epaperManager;

void setup()
{
  Serial.println(F("Setup started"));

  if (epaperManager.begin())
  {
    Serial.println(F("Begin successful, calling update"));
    epaperManager.update();
  }
  else
  {
    Serial.println(F("Begin failed"));
    // epd.Clear(EPD_7IN3E_WHITE);
    delay(30000);
    ESP.restart();
  }
}

void loop()
{
  // deepsleep
}