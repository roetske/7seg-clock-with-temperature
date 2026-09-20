/*espmini lolin wemos d1 r2 bord
 baudrate 9600
 18db20 temp sensor
 bij geen wifi ESP-Clock-Mini accespoint
 192.168.4.1 kom je op de webpagina voor wifi ssid
 display 2841bs-33 is common anode opgelet meeste zijn common kathode!!!
 code en aansluiting op de i2c driver ht16k zijn hier speciaal ook de code
 d1 d2 i2c
 d5 tempsensor
*/

#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <time.h>

// --- VERSIE INFORMATIE ---
#define FIRMWARE_VERSION "v5_DotTemp"

// --- HARDWARE CONFIGURATIE ---
#define HT16K33_ADDRESS 0x70
#define ONE_WIRE_BUS D5 // DS18B20 data pin op D5

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// --- WIFI & TIMING INSTELINGEN ---
unsigned long lastWiFiCheck = 0;
bool hasInitialTimeSync = false;

// --- DISPLAY BITMASKERS ---
const uint8_t charmap[] = {
  0x3F, // 0: '0'
  0x06, // 1: '1'
  0x5B, // 2: '2'
  0x4F, // 3: '3'
  0x66, // 4: '4'
  0x6D, // 5: '5'
  0x7D, // 6: '6'
  0x07, // 7: '7'
  0x7F, // 8: '8'
  0x6F, // 9: '9'
  0x39, // 10: 'C'
  0x63, // 11: '°'
  0x00, // 12: Blank / Spatie
  0x40, // 13: '-' (Streepje)
  0x54, // 14: 'n'
  0x5C, // 15: 'o'
  0x71  // 16: 'F'
};

uint8_t displayBuffer[4] = {13, 13, 13, 13}; // Start met '----'
bool showColon = false;       // Volledige dubbele punt (Klok)
bool showLowerDot = false;    // Enkel onderste punt onder de : (Temperatuur komma)

//***********************************
// --- DISPLAY STURING (Common Anode Transpositie voor NFD-2841Bx) ---
void updateDisplay() {
  uint8_t ram[16] = {0};

  for (int digit = 0; digit < 4; digit++) {
    uint8_t segments = charmap[displayBuffer[digit]];

    // 1. KLOKMODUS: Dubbele punt (D5 + D6) zit fysiek op DIG.2 -> in RAM op Digit 1
    if (digit == 1 && showColon) {
      segments |= 0x80; 
    }

    // 2. TEMPERATUURMODUS: Decimale punt D2 (onder de :) zit op DIG.1 -> in RAM op Digit 0
    if (digit == 0 && showLowerDot) {
      segments |= 0x80;
    }

    for (int seg = 0; seg < 8; seg++) {
      if (segments & (1 << seg)) {
        ram[seg * 2] |= (1 << digit);
      }
    }
  }

  Wire.beginTransmission(HT16K33_ADDRESS);
  Wire.write(0x00);
  for (uint8_t i = 0; i < 16; i++) {
    Wire.write(ram[i]);
  }
  Wire.endTransmission();
}
// --- HELDERHEID INSTELLEN (0x00 = Min, 0x0F = Max) ---
void setDisplayBrightness(uint8_t level) {
  if (level > 15) level = 15;
  Wire.beginTransmission(HT16K33_ADDRESS);
  Wire.write(0xE0 | level);
  Wire.endTransmission();
}

// --- SETUP ---
void setup() {
  Serial.begin(9600);
  delay(1000);

  Serial.println("\n=========================================");
  Serial.printf("   ESP8266 NTP Klok + DS18B20 %s   \n", FIRMWARE_VERSION);
  Serial.println("=========================================");

  // I2C start op D2 (SDA) en D1 (SCL)
  Wire.begin(D2, D1);
  Wire.setClockStretchLimit(1500);

  // HT16K33 Display initialiseren
  Wire.beginTransmission(HT16K33_ADDRESS); Wire.write(0x21); Wire.endTransmission(); // Oscillator AAN
  Wire.beginTransmission(HT16K33_ADDRESS); Wire.write(0x81); Wire.endTransmission(); // Display AAN
  setDisplayBrightness(15);

  updateDisplay(); // Toon direct '----'

  // Start Temperatuursensor (Asynchroon)
  sensors.begin();
  sensors.setWaitForConversion(false);

  // WiFiManager Setup
  WiFiManager wm;
  wm.setConfigPortalTimeout(180); // 3 min AP timeout bij opstarten
  
  Serial.println("[WIFI] Verbinden met netwerk...");
  if (!wm.autoConnect("ESP-Clock-Mini")) {
    Serial.println("[WIFI] Geen WiFi. Start op achtergrond, herstel volgt automatisch.");
  } else {
    Serial.println("[WIFI] Verbonden!");
    Serial.printf(" - IP: %s | RSSI: %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  }

  // Ingebouwde ESP8266 NTP Tijd / Tijdzone (CET/CEST voor BE/NL)
  configTime("CET-1CEST,M3.5.0,M10.5.0/3", "europe.pool.ntp.org", "time.nist.gov");
  Serial.println("[NTP] Tijdsynchronisatie gestart.");
}

// --- ACHTERGROND WIFI HERSTEL ---
void checkWiFiConnection() {
  unsigned long now = millis();
  
  if (now - lastWiFiCheck > 30000) {
    lastWiFiCheck = now;

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WIFI] Verbinding kwijt. Interne klok loopt door. Herverbinden...");
      WiFi.reconnect();
    }
  }
}

// --- MAIN LOOP ---
unsigned long previousMillis = 0;
const long interval = 1000; // 1 seconde tick
int secondsCounter = 0;

void loop() {
  checkWiFiConnection();

  unsigned long currentMillis = millis();

  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;
    secondsCounter++;

    bool isWiFiConnected = (WiFi.status() == WL_CONNECTED);
    int cycleLength = isWiFiConnected ? 10 : 12;

    // --- STAP 1: TIJD (0 tot 8 sec) ---
    if (secondsCounter <= 8) {
      showLowerDot = false; // Geen losse punt tijdens tijdsweergave
      
      time_t now = time(nullptr);
      struct tm* timeinfo = localtime(&now);

      if (timeinfo->tm_year > 120) {
        hasInitialTimeSync = true;
      }

      if (hasInitialTimeSync) {
        int hours = timeinfo->tm_hour;
        int minutes = timeinfo->tm_min;

        displayBuffer[0] = hours / 10;
        displayBuffer[1] = hours % 10;
        displayBuffer[2] = minutes / 10;
        displayBuffer[3] = minutes % 10;

        if (hours >= 23 || hours < 7) {
          setDisplayBrightness(1); 
        } else {
          setDisplayBrightness(15); 
        }

        showColon = !showColon; // Knipperen van de dubbele punt
      } else {
        displayBuffer[0] = 13; displayBuffer[1] = 13; displayBuffer[2] = 13; displayBuffer[3] = 13;
        showColon = false;
      }

      if (secondsCounter % 5 == 0 && hasInitialTimeSync) {
        Serial.printf("[STATUS %s] Tijd: %02d:%02d | WiFi: %s\n", 
                      FIRMWARE_VERSION, timeinfo->tm_hour, timeinfo->tm_min, 
                      isWiFiConnected ? "OK" : "OFFLINE");
      }

      if (secondsCounter == 8) {
        sensors.requestTemperatures();
      }

    // --- STAP 2: TEMPERATUUR MET 1 DECIEL EN 'C' (sec 9 en 10) ---
    } else if (secondsCounter <= 10) {
      showColon = false; // Dubbele punt (klok) uit
      
      float tempC = sensors.getTempCByIndex(0);

      if (tempC != DEVICE_DISCONNECTED_C && tempC > -50.0) {
        int tempScaled = (int)round(tempC * 10.0);

        if (tempScaled >= 0 && tempScaled <= 999) {
          // Bijv. 26.5°C -> Displays: [2] [6.] [5] [C]
          displayBuffer[0] = tempScaled / 100;        // Tientallen (2)
          displayBuffer[1] = (tempScaled / 10) % 10;  // Eenheden (6)
          displayBuffer[2] = tempScaled % 10;         // Tienden (5)
          displayBuffer[3] = 10;                      // 'C' (charmap index 10)
          
          showLowerDot = true; // Zet het puntje onder de dubbele punt AAN
        } 
        else if (tempScaled < 0 && tempScaled >= -99) {
          // Negatieve temperatuur: bijv. -3.4°C -> Displays: [-] [3.] [4] [C]
          int posTemp = abs(tempScaled);
          displayBuffer[0] = 13;                      // Minteken '-'
          displayBuffer[1] = posTemp / 10;            // Eenheden (3)
          displayBuffer[2] = posTemp % 10;            // Tienden (4)
          displayBuffer[3] = 10;                      // 'C'

          showLowerDot = true;
        }

        if (secondsCounter == 9) {
          Serial.printf("[STATUS %s] Temp (D5): %.1f °C\n", FIRMWARE_VERSION, tempC);
        }
      } else {
        showLowerDot = false;
        displayBuffer[0] = 13; displayBuffer[1] = 13; displayBuffer[2] = 13; displayBuffer[3] = 13;
      }

    // --- STAP 3: GEEN WIFI MELDING (sec 11 en 12) ---
    } else if (secondsCounter <= 12 && !isWiFiConnected) {
      showColon = false;
      showLowerDot = false;
      
      displayBuffer[0] = 14; // 'n'
      displayBuffer[1] = 15; // 'o'
      displayBuffer[2] = 13; // '-'
      displayBuffer[3] = 16; // 'F'

      if (secondsCounter == 11) {
        Serial.printf("[STATUS %s] Melding op display: no-F (WiFi offline)\n", FIRMWARE_VERSION);
      }
    }

    // Cyclus resetten
    if (secondsCounter >= cycleLength) {
      secondsCounter = 0;
    }

    // Ververs scherm
    updateDisplay();
  }
}