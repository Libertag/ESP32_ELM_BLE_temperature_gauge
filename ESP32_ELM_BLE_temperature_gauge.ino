#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEClient.h>
#include <TFT_eSPI.h> // Графическая библиотека для дисплея

// ***** Конфигурация BLE для ELM327 *****
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pRXCharacteristic = nullptr;
BLERemoteCharacteristic* pTXCharacteristic = nullptr;

String targetDeviceName = "IOS-Vlink";
String serviceUUID = "000018f0-0000-1000-8000-00805f9b34fb";
String rxUUID = "00002af0-0000-1000-8000-00805f9b34fb";
String txUUID = "00002af1-0000-1000-8000-00805f9b34fb";

String responseBuffer; // Буфер для накопления ответов ELM327
bool elmInitialized = false;
int engineTemp = 0; // Температура охлаждающей жидкости, полученная от ELM327

// ***** Графика и дисплей *****
TFT_eSPI tft = TFT_eSPI();  // Объект дисплея

// Загрузка изображений и др. ресурсов
#include "images.h"
#include "images_digits.h"
#include "images_needle.h"

// Переменные для отображения температуры
float temperature_sensor; 
int needle_image;
int gauge_min_value = 0; 
int gauge_max_value = 100; 
int value_temp_digits = 0;
float temperature_interpolated = 0;

// Период запроса температуры через ELM327
unsigned long lastElmRequest = 0;
unsigned long elmRequestInterval = 10000; // 10 секунд

// ***** CALLBACK для получения уведомлений от ELM327 *****
void notifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
  for (size_t i = 0; i < length; i++) {
    responseBuffer += (char)pData[i];
  }

  // Проверяем на '>'
  int promptIndex = responseBuffer.indexOf('>');
  if (promptIndex != -1) {
    String fullResponse = responseBuffer.substring(0, promptIndex);
    fullResponse.trim();
    responseBuffer = responseBuffer.substring(promptIndex + 1);

    if (fullResponse.length() > 0) {
      Serial.println("Ответ ELM327 (Complete): " + fullResponse);

      // Парсим, если это ответ на 0105
      if (fullResponse.indexOf("41 05") != -1) {
        int startIndex = fullResponse.indexOf("41 05");
        if (startIndex != -1) {
          String line = fullResponse.substring(startIndex);
          int firstSpace = line.indexOf(' ');
          int secondSpace = line.indexOf(' ', firstSpace + 1);
          int thirdSpace = line.indexOf(' ', secondSpace + 1);

          String hexValue;
          if (thirdSpace == -1) {
            hexValue = line.substring(secondSpace + 1);
          } else {
            hexValue = line.substring(secondSpace + 1, thirdSpace);
          }
          hexValue.trim();

          int rawVal = (int) strtol(hexValue.c_str(), NULL, 16);
          engineTemp = rawVal - 40;
          Serial.println("Температура охлаждающей жидкости (ELM): " + String(engineTemp) + " °C");
        }
      }
    }
  }
}

bool initializeELM() {
  Serial.println("Инициализация ELM327...");

  BLEScan* pScan = BLEDevice::getScan();
  pScan->setActiveScan(true);
  BLEScanResults* results = pScan->start(5);
  if (!results) {
    Serial.println("Не удалось получить результаты сканирования.");
    return false;
  }

  bool found = false;
  for (int i = 0; i < results->getCount(); i++) {
    BLEAdvertisedDevice device = results->getDevice(i);
    if (device.getName() == targetDeviceName) {
      Serial.println("Попытка подключения к устройству...");
      pClient = BLEDevice::createClient();
      if (pClient->connect(&device)) {
        Serial.println("Устройство подключено!");
        BLERemoteService* pService = pClient->getService(BLEUUID(serviceUUID));
        if (pService) {
          pRXCharacteristic = pService->getCharacteristic(BLEUUID(rxUUID));
          pTXCharacteristic = pService->getCharacteristic(BLEUUID(txUUID));

          if (pRXCharacteristic && pTXCharacteristic) {
            Serial.println("Характеристики RX/TX найдены!");
            if (pRXCharacteristic->canNotify()) {
              pRXCharacteristic->registerForNotify(notifyCallback);
            }
            found = true;
            break;
          } else {
            Serial.println("Не удалось найти RX/TX характеристики.");
          }
        } else {
          Serial.println("Не удалось подключиться к сервису.");
        }
      } else {
        Serial.println("Ошибка подключения.");
      }
    }
  }

  if (!found) {
    Serial.println("Устройство не найдено или не подключено.");
    return false;
  }

  // Последовательность инициализации ELM327
  Serial.println("Запрос версии ELM327 (ATI)...");
  pTXCharacteristic->writeValue("ATI\r\n");
  delay(2000);

  Serial.println("Отключаем эхо (ATE0)...");
  pTXCharacteristic->writeValue("ATE0\r\n");
  delay(2000);

  Serial.println("Отключаем выравнивание (ATL0)...");
  pTXCharacteristic->writeValue("ATL0\r\n");
  delay(2000);

  Serial.println("Выбираем протокол автоматически (ATSP0)...");
  pTXCharacteristic->writeValue("ATSP0\r\n");
  delay(2000);

  Serial.println("Запрашиваем текущий протокол (ATDP)...");
  pTXCharacteristic->writeValue("ATDP\r\n");
  delay(2000);

  elmInitialized = true;
  Serial.println("Инициализация ELM327 завершена.");
  return true;
}

void setup() {
  Serial.begin(115200);
  BLEDevice::init("ESP32_Client");

  if (!initializeELM()) {
    Serial.println("Не удалось инициализировать ELM327.");
  }

  // Инициализация дисплея
  tft.init(); 
  tft.setRotation(1); 
  tft.fillScreen(TFT_DARKGREY);
  tft.setTextFont(4); 
  tft.setSwapBytes(true); 
  tft.pushImage(0, 0, 240, 240, epd_bitmap_allArray[0]); // фоновое изображение
}

void loop() {
  if (elmInitialized && pClient && pClient->isConnected() && pTXCharacteristic) {
    // Запрашиваем температуру раз в 10 секунд
    unsigned long now = millis();
    if (now - lastElmRequest > elmRequestInterval) {
      lastElmRequest = now;
      Serial.println("Запрашиваем температуру охлаждающей жидкости (0105)...");
      responseBuffer = "";
      pTXCharacteristic->writeValue("0105\r\n");
      // Даем 3 секунды на получение ответа
      delay(3000);
    }

    // Обновляем отображение с текущим engineTemp
    temperature_sensor = engineTemp; 
    temperature_interpolated = temperature_interpolated + ((temperature_sensor - temperature_interpolated) / 5.0);
    value_temp_digits = round(temperature_interpolated);
    value_temp_digits = constrain(value_temp_digits, 0, 999);

    // Отрисовка цифр
    if (value_temp_digits < 10) {
      tft.pushImage(66, 181, 36, 44, bitmaps_digits[10]);
      tft.pushImage(102, 181, 36, 44, bitmaps_digits[value_temp_digits]);    
      tft.pushImage(138, 181, 36, 44, bitmaps_digits[10]);
    } else if (value_temp_digits < 100) {
      tft.pushImage(66, 181, 18, 44, bitmaps_digits[10]);
      tft.pushImage(84, 181, 36, 44, bitmaps_digits[(value_temp_digits % 100) / 10]);    
      tft.pushImage(120, 181, 36, 44, bitmaps_digits[value_temp_digits % 10]);
      tft.pushImage(156, 181, 18, 44, bitmaps_digits[10]);
    } else {
      tft.pushImage(66, 181, 36, 44, bitmaps_digits[value_temp_digits / 100]);
      tft.pushImage(102, 181, 36, 44, bitmaps_digits[(value_temp_digits % 100) / 10]);    
      tft.pushImage(138, 181, 36, 44, bitmaps_digits[value_temp_digits % 10]);
    }

    // Отрисовка стрелки
    needle_image = map(temperature_interpolated*10.0, gauge_min_value*10.0, gauge_max_value*10.0, 0*10.0, 120*10.0);
    needle_image = round(needle_image/10.0);
    needle_image = constrain(needle_image, 0, 120);
    tft.pushImage(11, 11, 218, 170, bitmaps_needle[needle_image]);

    
  } else {
    Serial.println("ELM327 не инициализирован или отключен.");
    delay(5000);
  }
}
