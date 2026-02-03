#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <WiFi.h>
#include <HTTPClient.h>

// --- CONFIGURAÇÕES DE REDE E SERVIDOR ---
const char* ssid = "SAAENOT001";
const char* password = "12345678";
const char* serverName = "http://192.168.175.16/ProjetoELM/receber_dados.php";

// --- CONFIGURAÇÕES DO VEÍCULO (Para o seu PHP) ---
String placaVeiculo = "BRA2E24"; 
int viagemID = 1; 

// UUIDs do adaptador (Baseado no seu LightBlue)
static BLEUUID serviceUUID("0000fff0-0000-1000-8000-00805f9b34fb");
static BLEUUID charUUID    ("0000fff1-0000-1000-8000-00805f9b34fb"); 

static boolean doConnect = false;
static boolean connected = false;
static BLERemoteCharacteristic* pRemoteCharacteristic;
static BLEAdvertisedDevice* myDevice;

// Variáveis de Sensores
float currentRPM = 0, currentKMH = 0, currentTemp = 0, currentVolt = 0;
unsigned long lastDbUpload = 0;
const int uploadInterval = 5000; 

// --- FUNÇÕES DE APOIO ---

long hexToDec(String hexString) {
  return strtol(hexString.c_str(), NULL, 16);
}

void enviarParaServidor() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(serverName);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    // Monta a string EXATAMENTE como o seu PHP espera no explode(',', $_POST['dados'])
    // viagem_id, placa, millis, rpm, velocidade, temperatura, voltagem
    String pacoteCSV = String(viagemID) + "," + 
                       placaVeiculo + "," + 
                       String(millis()) + "," + 
                       String(currentRPM) + "," + 
                       String(currentKMH) + "," + 
                       String(currentTemp) + "," + 
                       String(currentVolt);

    String httpRequestData = "dados=" + pacoteCSV;
    
    int httpResponseCode = http.POST(httpRequestData);
    Serial.print("HTTP Status Banco: "); Serial.println(httpResponseCode);
    
    if(httpResponseCode > 0) {
      Serial.println("Resposta PHP: " + http.getString());
    }
    http.end();
  }
}

// --- CALLBACK DE RECEBIMENTO ---

static void notifyCallback(BLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
    String response = "";
    for (int i = 0; i < length; i++) { response += (char)pData[i]; }
    response.trim();
    
    // Processa RPM (01 0C)
    if (response.indexOf("41 0C") != -1) {
        int pos = response.indexOf("41 0C") + 6;
        currentRPM = ((hexToDec(response.substring(pos, pos + 2)) * 256) + hexToDec(response.substring(pos + 3, pos + 5))) / 4.0;
    }
    // Processa Velocidade (01 0D)
    else if (response.indexOf("41 0D") != -1) {
        int pos = response.indexOf("41 0D") + 6;
        currentKMH = hexToDec(response.substring(pos, pos + 2));
    }
    // Processa Temperatura (01 05)
    else if (response.indexOf("41 05") != -1) {
        int pos = response.indexOf("41 05") + 6;
        currentTemp = hexToDec(response.substring(pos, pos + 2)) - 40;
    }
    // Processa Voltagem (Resposta ao comando AT RV)
    else if (response.length() > 0 && response.indexOf('.') != -1 && response.indexOf('V') != -1) {
        currentVolt = response.substring(0, response.indexOf('V')).toFloat();
    }

    // Saída para Monitor e Gráfico
    Serial.print("RPM:"); Serial.print(currentRPM);
    Serial.print(",Velocidade:"); Serial.print(currentKMH);
    Serial.print(",Temp:"); Serial.println(currentTemp);
}

// --- CLASSES DE CONEXÃO ---

class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) { connected = true; }
  void onDisconnect(BLEClient* pclient) { connected = false; Serial.println("Desconectado!"); }
};

class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(serviceUUID)) {
      BLEDevice::getScan()->stop();
      myDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

bool connectToServer() {
    BLEClient* pClient = BLEDevice::createClient();
    pClient->setClientCallbacks(new MyClientCallback());
    pClient->connect(myDevice);
    BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
    if (pRemoteService == nullptr) return false;
    pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUID);
    if (pRemoteCharacteristic == nullptr) return false;
    if(pRemoteCharacteristic->canNotify()) pRemoteCharacteristic->registerForNotify(notifyCallback);
    return true;
}

// --- SETUP E LOOP ---

void setup() {
  Serial.begin(115200);
  
  WiFi.begin(ssid, password);
  Serial.print("Conectando WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi OK!");

  BLEDevice::init("");
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true); 
  pBLEScan->start(15, false);
}

void loop() {
  if (doConnect) {
    if (connectToServer()) {
      Serial.println("Conectado ao Carro!");
      pRemoteCharacteristic->writeValue("ATZ\r", 4);    delay(1500);
      pRemoteCharacteristic->writeValue("ATE0\r", 5);   delay(500);
      pRemoteCharacteristic->writeValue("ATSP0\r", 6);  delay(1000);
    }
    doConnect = false;
  }

  if (connected) {
    // Sequência de leitura
    pRemoteCharacteristic->writeValue("010C\r", 5); delay(400); // RPM
    pRemoteCharacteristic->writeValue("010D\r", 5); delay(400); // Velocidade
    pRemoteCharacteristic->writeValue("0105\r", 5); delay(400); // Temperatura
    pRemoteCharacteristic->writeValue("ATRV\r", 5); delay(400); // Voltagem da Bateria

    // Envio para o Banco de Dados
    if (millis() - lastDbUpload > uploadInterval) {
        enviarParaServidor();
        lastDbUpload = millis();
    }
  } else {
    // Tenta reescanear se perder a conexão
    static unsigned long lastRetry = 0;
    if(millis() - lastRetry > 10000) {
      BLEDevice::getScan()->start(5, false);
      lastRetry = millis();
    }
  }
}