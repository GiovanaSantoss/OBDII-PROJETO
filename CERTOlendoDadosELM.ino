#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// UUIDs baseados no teu adaptador (Serviço FFF0)
static BLEUUID serviceUUID("0000fff0-0000-1000-8000-00805f9b34fb");
static BLEUUID charUUID    ("0000fff1-0000-1000-8000-00805f9b34fb"); 

static boolean doConnect = false;
static boolean connected = false;
static BLERemoteCharacteristic* pRemoteCharacteristic;
static BLEAdvertisedDevice* myDevice;

// Função para converter Hexadecimal para Decimal
long hexToDec(String hexString) {
  return strtol(hexString.c_str(), NULL, 16);
}

// Callback que processa as respostas do carro
static void notifyCallback(BLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
    String response = "";
    for (int i = 0; i < length; i++) {
        response += (char)pData[i];
    }
    response.trim();
    
    if (response.length() > 0) {
        // --- PROCESSAMENTO DO RPM (Comando 01 0C) ---
        // Resposta esperada contém "41 0C AA BB"
        if (response.indexOf("41 0C") != -1) {
            int pos = response.indexOf("41 0C") + 6;
            String hexA = response.substring(pos, pos + 2);
            String hexB = response.substring(pos + 3, pos + 5);
            
            int rpm = ((hexToDec(hexA) * 256) + hexToDec(hexB)) / 4;
            Serial.print("[MOTOR] RPM: ");
            Serial.println(rpm);
        }

        // --- PROCESSAMENTO DA VELOCIDADE (Comando 01 0D) ---
        // Resposta esperada contém "41 0D VV"
        if (response.indexOf("41 0D") != -1) {
            int pos = response.indexOf("41 0D") + 6;
            String hexV = response.substring(pos, pos + 2);
            
            int kmh = hexToDec(hexV);
            Serial.print("[CARRO] Velocidade: ");
            Serial.print(kmh);
            Serial.println(" km/h");
        }
    }
}

class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) { connected = true; }
  void onDisconnect(BLEClient* pclient) { connected = false; Serial.println("Desconectado!"); }
};

bool connectToServer() {
    Serial.print("A conectar a: ");
    Serial.println(myDevice->getAddress().toString().c_str());
    
    BLEClient* pClient  = BLEDevice::createClient();
    pClient->setClientCallbacks(new MyClientCallback());
    pClient->connect(myDevice);

    BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
    if (pRemoteService == nullptr) return false;

    pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUID);
    if (pRemoteCharacteristic == nullptr) return false;

    if(pRemoteCharacteristic->canNotify())
      pRemoteCharacteristic->registerForNotify(notifyCallback);

    return true;
}

class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(serviceUUID)) {
      BLEDevice::getScan()->stop();
      myDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

void setup() {
  Serial.begin(115200);
  Serial.println("Iniciando busca OBD-II...");
  
  BLEDevice::init("");
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setInterval(1349);
  pBLEScan->setWindow(449);
  pBLEScan->setActiveScan(true); 
  pBLEScan->start(10, false);
}

void loop() {
  if (doConnect) {
    if (connectToServer()) {
      Serial.println("Conectado!");
      // Configuração inicial do chip ELM327
      pRemoteCharacteristic->writeValue("ATZ\r", 4);    delay(500); // Reset
      pRemoteCharacteristic->writeValue("ATE0\r", 5);   delay(500); // Eco OFF
      pRemoteCharacteristic->writeValue("ATSP0\r", 6);  delay(500); // Protocolo Auto
    }
    doConnect = false;
  }

  if (connected) {
    // Pede RPM (PID 010C)
    pRemoteCharacteristic->writeValue("010C\r", 5);
    delay(500);
    
    // Pede Velocidade (PID 010D)
    pRemoteCharacteristic->writeValue("010D\r", 5);
    delay(500);
  }
}