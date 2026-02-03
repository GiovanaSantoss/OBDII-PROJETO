#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// UUIDs baseados no seu adaptador (Serviço FFF0)
static BLEUUID serviceUUID("0000fff0-0000-1000-8000-00805f9b34fb");
static BLEUUID charUUID    ("0000fff1-0000-1000-8000-00805f9b34fb"); 

static boolean doConnect = false;
static boolean connected = false;
static BLERemoteCharacteristic* pRemoteCharacteristic;
static BLEAdvertisedDevice* myDevice;

// Variáveis para manter os últimos valores lidos (para o gráfico não zerar a outra linha)
int currentRPM = 0;
int currentKMH = 0;

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
        // PROCESSAMENTO DO RPM (PID 01 0C)
        if (response.indexOf("41 0C") != -1) {
            int pos = response.indexOf("41 0C") + 6;
            if (response.length() >= pos + 5) {
                String hexA = response.substring(pos, pos + 2);
                String hexB = response.substring(pos + 3, pos + 5);
                currentRPM = ((hexToDec(hexA) * 256) + hexToDec(hexB)) / 4;
            }
        }

        // PROCESSAMENTO DA VELOCIDADE (PID 01 0D)
        if (response.indexOf("41 0D") != -1) {
            int pos = response.indexOf("41 0D") + 6;
            if (response.length() >= pos + 2) {
                String hexV = response.substring(pos, pos + 2);
                currentKMH = hexToDec(hexV);
            }
        }

        // SAÍDA PARA O SERIAL PLOTTER (Gráfico)
        // O formato "Legenda:Valor" permite que o gráfico mostre os nomes das variáveis
        Serial.print("RPM:");
        Serial.print(currentRPM);
        Serial.print(",");
        Serial.print("Velocidade_KMH:");
        Serial.println(currentKMH);
    }
}

class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) { connected = true; }
  void onDisconnect(BLEClient* pclient) { 
    connected = false; 
    Serial.println("Conexão com OBD-II perdida!"); 
  }
};

bool connectToServer() {
    Serial.print("Tentando conectar ao dispositivo: ");
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
  Serial.println("--- Iniciando Leitor OBD-II com Gráfico ---");
  
  BLEDevice::init("");
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setInterval(1349);
  pBLEScan->setWindow(449);
  pBLEScan->setActiveScan(true); 
  pBLEScan->start(15, false);
}

void loop() {
  if (doConnect) {
    if (connectToServer()) {
      Serial.println("Conectado ao Carro!");
      // Configuração inicial do ELM327
      pRemoteCharacteristic->writeValue("ATZ\r", 4);    delay(500);
      pRemoteCharacteristic->writeValue("ATE0\r", 5);   delay(500); // Desliga o eco para limpar o gráfico
      pRemoteCharacteristic->writeValue("ATSP0\r", 6);  delay(500);
    }
    doConnect = false;
  }

  if (connected) {
    // Solicita RPM
    pRemoteCharacteristic->writeValue("010C\r", 5);
    delay(150); // Delay curto para o gráfico ser fluido
    
    // Solicita Velocidade
    pRemoteCharacteristic->writeValue("010D\r", 5);
    delay(150);
  } else {
    // Se não estiver conectado, tenta escanear novamente a cada 10 segundos
    static unsigned long lastScan = 0;
    if (millis() - lastScan > 10000) {
        BLEDevice::getScan()->start(5, false);
        lastScan = millis();
    }
  }
}
//teste 