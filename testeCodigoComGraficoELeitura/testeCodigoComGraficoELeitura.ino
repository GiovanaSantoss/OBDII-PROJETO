#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <WiFi.h>
#include <HTTPClient.h> 
#include <SPI.h>
#include "SdFat.h"

// --- PINOUT TTGO LORA32 (O que funcionou) ---
#define SD_CS    13
#define SD_MOSI  15
#define SD_MISO  2
#define SD_SCK   14

// --- CONFIGURAÇÕES DE REDE E ARQUIVO ---
const char* ssid = "SAAENOT001";
const char* password = "12345678";
const char* serverUrl = "http://192.168.175.16/ProjetoELM/receber_dados.php";
#define ARQUIVO_CARRO "/dados_carros.csv"

// --- VARIÁVEIS BLE ---
static BLEUUID serviceUUID("0000fff0-0000-1000-8000-00805f9b34fb");
static BLEUUID charUUID    ("0000fff1-0000-1000-8000-00805f9b34fb"); 
static boolean connected = false;
static boolean doConnect = false;
static BLERemoteCharacteristic* pRemoteCharacteristic;
static BLEAdvertisedDevice* myDevice;

// Dados dos Sensores
float currentRPM = 0, currentKMH = 0, currentTemp = 0, currentVolt = 0;
String placaVeiculo = "BRA2E24";
int viagemID = 1;

// --- TIMERS DE CONTROLE ---
unsigned long wifiDownTimer = 0;
unsigned long wifiUpTimer = 0;
unsigned long lastLeituraELM = 0;
const unsigned long TEMPO_VALIDACAO = 60000; // 1 minuto (Mude para 10000 para testar rápido)
bool sdFuncional = false;

SPIClass sdSPI(HSPI);
SdFat sd;

// --- PROTÓTIPOS DE FUNÇÕES (Para evitar erro de escopo) ---
void processarLogicaEscritaEEnvio();
String montarPacoteCSV();
void enviarParaServidor(String pacote);
void gravarNoSD(String dados);
void descarregarSD();

// --- FUNÇÕES DE APOIO ---

long hexToDec(String hexString) { return strtol(hexString.c_str(), NULL, 16); }

void iniciarSD() {
    sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (sd.begin(SdSpiConfig(SD_CS, DEDICATED_SPI, SD_SCK_MHZ(4), &sdSPI))) {
        Serial.println("[SD] Cartão Detectado com Sucesso!");
        sdFuncional = true;

        if (!sd.exists(ARQUIVO_CARRO)){
            FsFile file;
            if (file.open(ARQUIVO_CARRO, O_WRONLY | O_CREAT)){
                file.println("Viagem_ID;Placa;Timestamp_ms;RPM;Velocidade_KMH;Temp_Agua;Voltagem");
                file.close();
                Serial.println("[SD] Novo arquivo criado com cabeçalho");
            }
        }
    } else {
        sdFuncional = false;
        Serial.println("[SD] Erro ao iniciar cartão!");
    }
}

String montarPacoteCSV() {
    return String(viagemID) + ";" + placaVeiculo + ";" + String(millis()) + ";" + 
           String(currentRPM) + ";" + String(currentKMH) + ";" + 
           String(currentTemp) + ";" + String(currentVolt);
}

void gravarNoSD(String dados) {
    if(!sdFuncional) {
        Serial.println("[SD] Tentando detectar cartão...");
        iniciarSD();
    }

    if(!sdFuncional) {
        return;
    }

    FsFile file;

    if (!file.open(ARQUIVO_CARRO, O_RDWR| O_CREAT | O_AT_END)){
        Serial.println("[SD] Erro ao abrir arquivo para escrita.");
        Serial.println("[SD] Tentando resetar sistema de arquivos...");

        sd.begin(SdSpiConfig(SD_CS, DEDICATED_SPI, SD_SCK_MHZ(4), &sdSPI));

        if(!file.open(ARQUIVO_CARRO, O_RDWR | O_CREAT | O_AT_END)){
            Serial.println("[SD] Falha persistente. Verifique o cartão.");
            sdFuncional = false;
            return;
        }
    }

    file.println(dados);
    file.close();
    Serial.println("[SD] Dados gravados com sucesso!");
}

void enviarParaServidor(String pacote) {
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        http.begin(serverUrl);
        http.addHeader("Content-Type", "application/x-www-form-urlencoded");
        int res = http.POST("dados=" + pacote);
        if(res > 0) Serial.println("[WEB] Enviado com sucesso!");
        else Serial.println("[WEB] Erro HTTP: " + String(res)); // Verifique se dá -1 (timeout)
        http.end();
    } else {
        Serial.println("[WEB] WiFi Desconectado no momento do envio!");
    }
}

void descarregarSD() {
    if (!sd.exists(ARQUIVO_CARRO)) return;
    Serial.println("[SYNC] Descarregando SD...");
    FsFile file;
    if (file.open(ARQUIVO_CARRO, O_RDONLY)) {
        char linha[128];
        while (file.fgets(linha, sizeof(linha))) {
            String sLinha = String(linha);
            sLinha.trim();
            if (sLinha.length() > 0) {
                enviarParaServidor(sLinha);
                delay(100);
            }
        }
        file.close();
        sd.remove(ARQUIVO_CARRO);
        Serial.println("[SYNC] SD Limpo!");
    }
}

// --- CALLBACKS BLE ---
static void notifyCallback(BLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
    String resp = "";
    for (int i = 0; i < length; i++) resp += (char)pData[i];
    resp.trim();
    
    if (resp.indexOf("41 0C") != -1) {
        int pos = resp.indexOf("41 0C") + 6;
        currentRPM = ((hexToDec(resp.substring(pos, pos + 2)) * 256) + hexToDec(resp.substring(pos + 3, pos + 5))) / 4.0;
    } else if (resp.indexOf("41 0D") != -1) {
        currentKMH = hexToDec(resp.substring(resp.indexOf("41 0D") + 6, resp.indexOf("41 0D") + 8));
    } else if (resp.indexOf("41 05") != -1) {
        currentTemp = hexToDec(resp.substring(resp.indexOf("41 05") + 6, resp.indexOf("41 05") + 8)) - 40;
    } else if (resp.length() > 0 && resp.indexOf('.') != -1 && resp.indexOf('V') != -1) {
        currentVolt = resp.substring(0, resp.indexOf('V')).toFloat();
    }
}

class MyClientCallback : public BLEClientCallbacks {
    void onConnect(BLEClient* p) { connected = true; Serial.println("Conectado ao Carro!"); }
    void onDisconnect(BLEClient* p) { connected = false; Serial.println("Carro Desconectado!"); }
};

class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice adv) {
        if (adv.isAdvertisingService(serviceUUID)) {
            BLEDevice::getScan()->stop();
            myDevice = new BLEAdvertisedDevice(adv);
            doConnect = true;
        }
    }
};

// --- FUNÇÃO DE LÓGICA CENTRAL ---
void processarLogicaEscritaEEnvio() {
    bool wifiOk = (WiFi.status() == WL_CONNECTED);

    if (!wifiOk) {
        wifiUpTimer = 0;
        if (wifiDownTimer == 0) wifiDownTimer = millis();

        if (millis() - wifiDownTimer > TEMPO_VALIDACAO) {
            gravarNoSD(montarPacoteCSV());
        } else {
            Serial.print("WiFi Caído. Gravando no SD em: ");
            Serial.print((TEMPO_VALIDACAO - (millis() - wifiDownTimer)) / 1000);
            Serial.println("s");
        }
    } 
    else {
        wifiDownTimer = 0;
        if (wifiUpTimer == 0) wifiUpTimer = millis();

        // Envia dado atual em tempo real
        enviarParaServidor(montarPacoteCSV());

        // Se WiFi estável por 1min e carro parado, descarrega o SD
        if ((millis() - wifiUpTimer > TEMPO_VALIDACAO) && (currentKMH <= 0.5)) {
            descarregarSD();
            wifiUpTimer = millis(); // Reseta para não repetir o loop imediatamente
        } else if (millis() - wifiUpTimer < TEMPO_VALIDACAO) {
            Serial.print("WiFi OK. Estabilizando para sincronizar SD em: ");
            Serial.print((TEMPO_VALIDACAO - (millis() - wifiUpTimer)) / 1000);
            Serial.println("s");
        }
    }
}

// --- SETUP ---
void setup() {
    Serial.begin(115200);
    pinMode(18, OUTPUT); digitalWrite(18, HIGH); 
    
    iniciarSD();

    if (sdFuncional && !sd.exists(ARQUIVO_CARRO)){
      FsFile file;

      if(file.open(ARQUIVO_CARRO, O_WRONLY | O_CREAT)){
        file.println("Viagem_ID, Placa, Timestamp_ms, RPM, Velocidade_KMH, Temp_Agua, Voltagem");
        file.close();
        Serial.println("[SD] Arquivo novo criado com cabeçalho.");
      }
    }

    WiFi.begin(ssid, password);
    Serial.println("Iniciando WiFi...");
    
    BLEDevice::init("");
    BLEScan* pScan = BLEDevice::getScan();
    pScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
    pScan->start(15, false);
}

// --- LOOP ---
void loop() {
    if (doConnect) {
        BLEClient* pClient = BLEDevice::createClient();
        pClient->setClientCallbacks(new MyClientCallback());
        if (pClient->connect(myDevice)) {
            BLERemoteService* pSvc = pClient->getService(serviceUUID);
            if (pSvc) {
                pRemoteCharacteristic = pSvc->getCharacteristic(charUUID);
                if (pRemoteCharacteristic && pRemoteCharacteristic->canNotify()) {
                    pRemoteCharacteristic->registerForNotify(notifyCallback);
                    pRemoteCharacteristic->writeValue("ATZ\r", 4);    delay(1000);
                    pRemoteCharacteristic->writeValue("ATE0\r", 5);   delay(500);
                    pRemoteCharacteristic->writeValue("ATSP0\r", 6);  delay(500);
                }
            }
        }
        doConnect = false;
    }

    // MODO SIMULAÇÃO (Se o carro estiver desconectado)
    static unsigned long lastFakeData = 0;
    if (!connected && (millis() - lastFakeData > 4000)){
        lastFakeData = millis();
        currentRPM = 1500 + random(0, 500);
        currentKMH = 60 + random(0, 10); // Mude para 0 para testar o descarregamento do SD
        currentTemp = 90;
        currentVolt = 13.8;

        Serial.println("\n--- MODO SIMULAÇÃO (Sem ELM327) ---");
        processarLogicaEscritaEEnvio();
    }

    // MODO REAL (Se o carro conectar)
    if (connected) {
        if (millis() - lastLeituraELM > 5000) {
            lastLeituraELM = millis();
            pRemoteCharacteristic->writeValue("010C\r", 5); delay(300);
            pRemoteCharacteristic->writeValue("010D\r", 5); delay(300);
            pRemoteCharacteristic->writeValue("0105\r", 5); delay(300);
            pRemoteCharacteristic->writeValue("ATRV\r", 5); delay(300);

            delay(500);

            Serial.println("\n--- MODO REAL (Lendo ELM327) ---");
            processarLogicaEscritaEEnvio();
        }
    } else {
        // Tenta re-escanear se perder a conexão
        static unsigned long lastRetry = 0;
        if(millis() - lastRetry > 15000) {
            BLEDevice::getScan()->start(5, false);
            lastRetry = millis();
        }
    }
}