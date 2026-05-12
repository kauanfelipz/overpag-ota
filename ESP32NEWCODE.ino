#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h> 
#include <LiquidCrystal_I2C.h>
#include <Preferences.h> 
#include <ArduinoJson.h>
#include <WiFiManager.h> 
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>

// ============================================================================
// CONFIGURAÇÕES DE SERVIDOR E MÁQUINA
// ============================================================================
const char* servidorAPI = "https://api-overpag.onrender.com";

// 👇 SEU ID FIXO FICA AQUI 👇
const char* idMaquina = "maquina01"; 

String urlVerificarPagamento;

// ============================================================================
// CONFIGURAÇÕES DE PINOS
// ============================================================================
const int PINO_RELAY_BOMBA = 4;      
const int PINO_ILUMINACAO = 19;      
const int PINO_LED_STATUS = 2; 
const int PINO_BUZZER = 5; 
const int PINO_MOEDEIRO = 23;

// Frequência do PWM (Versão ESP32 3.0+)
const int LEDC_FREQ = 5000;
const int LEDC_RESOLUTION = 8; 

// ============================================================================
// CONFIGURAÇÕES DE TEMPO E VALOR
// ============================================================================
float precoPixAtual = 2.00;            
unsigned long tempoBaseSegundos = 240; 

const long TEMPO_LUZ_MS = 360000;      
const long INTERVALO_HTTP = 3000;      
const long INTERVALO_TELA = 3000;      
const long TIMEOUT_HTTP_MS = 8000;     

// ============================================================================
// VARIÁVEIS GLOBAIS
// ============================================================================
volatile int contadorMoedas = 0;           
volatile unsigned long ultimoTempoMoeda = 0; 
unsigned long ultimoCheckHTTP = 0;
unsigned long ultimoTentativaWiFi = 0; 

unsigned long inicioLuz = 0;
unsigned long duracaoLuz = 0;
bool luzLigada = false;

// Controle de bip das moedas
bool bipMoedaAtivo = false;
unsigned long tempoMoedaBip = 0;

Preferences preferencias;
unsigned int contadorTotal = 0;  
unsigned long tempoUltimaTrocaTela = 0;
bool mostrandoValor = true;

// Controle PIX duplicado
int ultimoIdPixProcessado = -1;
unsigned long tempoUltimoPixProcessado = 0;
const long TEMPO_EXPIRACAO_PIX_MS = 300000; 

bool bombaEmOperacao = false;

LiquidCrystal_I2C lcd(0x27, 16, 2);

// ============================================================================
// FUNÇÕES AUXILIARES
// ============================================================================
void escreverCentralizado(String texto, int linha) {
  int paddingEsq = (16 - texto.length()) / 2;
  if (paddingEsq < 0) paddingEsq = 0;
  
  String textoLimpo = "";
  for (int i = 0; i < paddingEsq; i++) textoLimpo += " ";
  textoLimpo += texto;
  while (textoLimpo.length() < 16) {
    textoLimpo += " ";
  }

  lcd.setCursor(0, linha);
  lcd.print(textoLimpo);
}

void IRAM_ATTR contarMoeda() {
  unsigned long tempoAtual = millis();
  if (tempoAtual - ultimoTempoMoeda > 150) { 
    contadorMoedas++;
    ultimoTempoMoeda = tempoAtual;
  }
}

int calcularPorcentagemWiFi(long rssi) {
  if (rssi <= -100) return 0;
  if (rssi >= -50) return 100;
  return 2 * (rssi + 100);
}

void construirURLs() {
  urlVerificarPagamento = String(servidorAPI) + "/verificar_pagamento/" + String(idMaquina);
}

void salvarPixProcessado(int idPix) {
  ultimoIdPixProcessado = idPix;
  tempoUltimoPixProcessado = millis();
  preferencias.putInt("ultimo_pix", idPix);
  preferencias.putULong("tempo_ultimo_pix", tempoUltimoPixProcessado);
}

bool pixJaProcessado(int idPix) {
  if (idPix == ultimoIdPixProcessado) {
    if (millis() - tempoUltimoPixProcessado < TEMPO_EXPIRACAO_PIX_MS) return true;
  }
  return false;
}

void telaConfiguracaoWiFi(WiFiManager *myWiFiManager) {
  lcd.clear();
  escreverCentralizado("Modo Instalador", 0);
  escreverCentralizado("Rede: OverPag", 1);
  Serial.println("Aguardando celular configurar...");
}

// ============================================================================
// COMUNICAÇÃO (OTA E MOEDAS)
// ============================================================================
void realizarAtualizacaoOTA(String urlArquivoBin) {
  Serial.println("Iniciando atualizacao OTA...");
  lcd.clear();
  escreverCentralizado("Atualizando...", 0);
  escreverCentralizado("Aguarde...", 1);

  WiFiClientSecure client;
  client.setInsecure(); 

  t_httpUpdate_return ret = httpUpdate.update(client, urlArquivoBin);

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("Erro no OTA (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
      lcd.clear();
      escreverCentralizado("Erro ao Baixar", 0);
      delay(3000);
      lcd.clear();
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("Nenhuma atualizacao encontrada.");
      break;
    case HTTP_UPDATE_OK:
      Serial.println("Sucesso! Reiniciando...");
      ESP.restart();
      break;
  }
}

void avisarServidorMoedasFisicas(String id, float valor) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String urlPost = String(servidorAPI) + "/registrar_moeda/" + id;
    http.begin(urlPost);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(3000); 
    String payload = "{\"valor\":" + String(valor) + "}";
    http.POST(payload);
    http.end();
  }
}

bool consultarServidor() {
  if (bombaEmOperacao) return false; 
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  
  if (!http.begin(client, urlVerificarPagamento)) return false;
  http.setTimeout(TIMEOUT_HTTP_MS);

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    StaticJsonDocument<1536> doc; 
    
    if (!deserializeJson(doc, payload)) {
      if (doc.containsKey("preco_pix")) precoPixAtual = doc["preco_pix"].as<float>();
      if (doc.containsKey("tempo_segundos")) tempoBaseSegundos = doc["tempo_segundos"].as<int>();
      
      if (doc.containsKey("url_ota")) {
        String urlNova = doc["url_ota"].as<String>();
        if (urlNova.length() > 10) realizarAtualizacaoOTA(urlNova);
      }

      if (doc.containsKey("status") && doc["status"].as<String>() == "aprovado") {
        int idPix = doc["id_pix"].as<int>();
        
        if (!pixJaProcessado(idPix)) {
          salvarPixProcessado(idPix);
          http.end();
          
          int tempoLiberadoSec = doc["tempo_liberado"].as<int>();
          Serial.println("PIX Aprovado!");
          bombaEmOperacao = true;
          ligarBombaComTimer(tempoLiberadoSec * 1000UL);
          bombaEmOperacao = false;
          return true;
        }
      }
    }
  }
  http.end();
  return false;
}

// ============================================================================
// LÓGICA DA BOMBA
// ============================================================================
void ligarBombaComTimer(unsigned long tempoTotalMillis) {
  Serial.println("Ligando Bomba...");
  
  luzLigada = true;
  inicioLuz = millis();         
  duracaoLuz = TEMPO_LUZ_MS;    
  digitalWrite(PINO_ILUMINACAO, HIGH); 
  digitalWrite(PINO_RELAY_BOMBA, HIGH);
  
  unsigned long tempoInicio = millis();
  unsigned long ultimaAtualizacaoLCD = 0;
  
  lcd.clear();
  escreverCentralizado("BOMBA LIGADA!", 0);

  while ((millis() - tempoInicio) < tempoTotalMillis) {
    
    int moedasLidas = 0;
    if (contadorMoedas > 0) {
      noInterrupts(); 
      moedasLidas = contadorMoedas;
      contadorMoedas = 0; 
      interrupts(); 
    }

    if (moedasLidas > 0) {
      float valorInserido = moedasLidas * 1.00;
      unsigned long tempoExtraMillis = ((tempoBaseSegundos / precoPixAtual) * valorInserido) * 1000UL;
      tempoTotalMillis += tempoExtraMillis;
      
      // SOMA SÓ AS MOEDAS REAIS NO ENCERRANTE
      contadorTotal += moedasLidas;
      preferencias.putUInt("total", contadorTotal);

      avisarServidorMoedasFisicas(String(idMaquina), valorInserido);
      
      tone(PINO_BUZZER, 1200);
      bipMoedaAtivo = true;
      tempoMoedaBip = millis();
    }

    if (bipMoedaAtivo && (millis() - tempoMoedaBip >= 150)) {
      noTone(PINO_BUZZER);
      bipMoedaAtivo = false;
    }

    unsigned long tempoPassado = millis() - tempoInicio;
    unsigned long tempoRestante = tempoTotalMillis - tempoPassado;

    // --- NOVO PWM ESP32 v3.0+ ---
    float angulo = (float)millis() / 500.0; 
    int brilho = 128 + 127 * sin(angulo);   
    ledcWrite(PINO_LED_STATUS, brilho);    

    if (millis() - ultimaAtualizacaoLCD >= 1000) {
      ultimaAtualizacaoLCD = millis();
      int minutos = (tempoRestante / 1000) / 60;
      int segundos = (tempoRestante / 1000) % 60;

      char buf[17];
      sprintf(buf, "Tempo: %02d:%02d", minutos, segundos);
      escreverCentralizado(String(buf), 1);
    }

    if (tempoRestante <= 10000) {
      if ((millis() % 1000) < 200) tone(PINO_BUZZER, 1000); 
      else if (!bipMoedaAtivo) noTone(PINO_BUZZER); 
    }
    
    delay(10); 
  }

  // 👇 PROTEÇÃO ANTI-FANTASMA ANTES DE DESLIGAR O MOTOR 👇
  detachInterrupt(digitalPinToInterrupt(PINO_MOEDEIRO)); 

  digitalWrite(PINO_RELAY_BOMBA, LOW);  // Coice do motor isolado
  
  // --- NOVO PWM ESP32 v3.0+ ---
  ledcWrite(PINO_LED_STATUS, 0); 
  noTone(PINO_BUZZER);                
  
  delay(100); // Aguarda poeira baixar
  contadorMoedas = 0; // Limpa ruído
  attachInterrupt(digitalPinToInterrupt(PINO_MOEDEIRO), contarMoeda, FALLING); // Religa ouvidos
  // 👆 ================================================= 👆

  lcd.clear();
  escreverCentralizado(" TEMPO ACABOU! ", 0);
  delay(3000);
  lcd.clear();
}

// ============================================================================
// SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);
  
  pinMode(PINO_RELAY_BOMBA, OUTPUT);
  pinMode(PINO_ILUMINACAO, OUTPUT); 
  pinMode(PINO_BUZZER, OUTPUT); 
  
  pinMode(PINO_MOEDEIRO, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PINO_MOEDEIRO), contarMoeda, FALLING);
  
  // --- NOVO PWM ESP32 v3.0+ ---
  ledcAttach(PINO_LED_STATUS, LEDC_FREQ, LEDC_RESOLUTION);
  ledcWrite(PINO_LED_STATUS, 0);
  
  digitalWrite(PINO_RELAY_BOMBA, LOW); 
  digitalWrite(PINO_ILUMINACAO, LOW); 
  noTone(PINO_BUZZER); 

  preferencias.begin("maquina", false); 
  contadorTotal = preferencias.getUInt("total", 0); 
  ultimoIdPixProcessado = preferencias.getInt("ultimo_pix", -1);
  tempoUltimoPixProcessado = preferencias.getULong("tempo_ultimo_pix", 0);

  lcd.init(); 
  lcd.backlight();
  escreverCentralizado("Iniciando...", 0);
  delay(1000);

  WiFi.mode(WIFI_STA); 
  WiFiManager wifiManager;
  wifiManager.setConfigPortalTimeout(180); 
  wifiManager.setAPCallback(telaConfiguracaoWiFi);

  if (!wifiManager.autoConnect("OverPag", "admin123")) {
    lcd.clear();
    escreverCentralizado("PIX INDISPONIVEL", 0);
    escreverCentralizado("SO ACEITA MOEDAS", 1);
    delay(3000);
  } else {
    int sinal = calcularPorcentagemWiFi(WiFi.RSSI());
    lcd.clear();
    escreverCentralizado("WiFi: " + String(sinal) + "%", 0);
    escreverCentralizado(sinal > 50 ? "Sinal Bom" : "Sinal Fraco", 1);
    delay(4000);
    
    lcd.clear();
    escreverCentralizado("Conectado!", 0);
    delay(2000);
  }

  construirURLs();
  lcd.clear();
}

// ============================================================================
// LOOP PRINCIPAL
// ============================================================================
void loop() {
  
  if (luzLigada) {
    if (millis() - inicioLuz >= duracaoLuz) {
      luzLigada = false;
      digitalWrite(PINO_ILUMINACAO, LOW); 
      duracaoLuz = 0;
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - ultimoTentativaWiFi >= 10000) { 
      WiFi.disconnect();
      WiFi.reconnect();
      ultimoTentativaWiFi = millis();
    }
  }

  int moedasLidas = 0;
  if (contadorMoedas > 0) {
    noInterrupts();
    moedasLidas = contadorMoedas;
    interrupts();
  }

  if (moedasLidas == 0) {
    if (WiFi.status() == WL_CONNECTED) {
      escreverCentralizado("Auto Posto", 0); 

      if (millis() - tempoUltimaTrocaTela > INTERVALO_TELA) {
        mostrandoValor = !mostrandoValor; 
        tempoUltimaTrocaTela = millis();
      }

      char bufTela[17];
      if (mostrandoValor) {
        sprintf(bufTela, "Valor: R$ %.2f", precoPixAtual);
      } else {
        sprintf(bufTela, "Cont: %05u", contadorTotal); // Tela do frentista limpa!
      }
      escreverCentralizado(String(bufTela), 1);
      
      if (!bombaEmOperacao && (millis() - ultimoCheckHTTP >= INTERVALO_HTTP)) {
        ultimoCheckHTTP = millis();
        consultarServidor();
      }
      
    } else {
      escreverCentralizado("PIX INDISPONIVEL", 0);
      escreverCentralizado("SO ACEITA MOEDAS", 1);
    }
  } 
  else {
    unsigned long tempoEspera = millis() - ultimoTempoMoeda;
    
    // Aguarda apenas MEIO SEGUNDO (500ms) para o pulso elétrico estabilizar e já liga!
    if (tempoEspera >= 500) { 
      float valorInserido = moedasLidas * 1.00;
      unsigned long tempoProporcionalSegundos = (tempoBaseSegundos / precoPixAtual) * valorInserido;

      noInterrupts();
      contadorMoedas = 0; 
      interrupts();

      // SOMA SÓ AS MOEDAS REAIS NO ENCERRANTE
      contadorTotal += moedasLidas;
      preferencias.putUInt("total", contadorTotal);
      
      avisarServidorMoedasFisicas(String(idMaquina), valorInserido);
      
      tone(PINO_BUZZER, 800);
      delay(200);
      noTone(PINO_BUZZER);
      
      bombaEmOperacao = true;
      ligarBombaComTimer(tempoProporcionalSegundos * 1000UL);
      bombaEmOperacao = false;
      lcd.clear(); 
    }
  }
  
  delay(10); 
}
