#include "esp_camera.h"
#include "esp_http_server.h"
#include "FS.h"
#include "SD_MMC.h"
#include <WiFi.h>

// ========== CONFIGURATION ==========
const char* WIFI_SSID     = "MonESP8266";
const char* WIFI_PASSWORD = "";
const char* SERVER_HOST   = "192.168.4.10";
const uint16_t SERVER_PORT = 8888;

#define BOUTON_PIN 13
#define FLASH_PIN   4


#define JPEG_QUALITY 6
#define FRAME_SIZE FRAMESIZE_SVGA   

// Délais stables
#define FLASH_DELAY_MS  150
#define TCP_TIMEOUT_MS  3000
#define DEBOUNCE_MS     1500

// ========== PINS CAMERA ==========
#define PWDN_GPIO_NUM  32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM   0
#define SIOD_GPIO_NUM  26
#define SIOC_GPIO_NUM  27
#define Y9_GPIO_NUM    35
#define Y8_GPIO_NUM    34
#define Y7_GPIO_NUM    39
#define Y6_GPIO_NUM    36
#define Y5_GPIO_NUM    21
#define Y4_GPIO_NUM    19
#define Y3_GPIO_NUM    18
#define Y2_GPIO_NUM     5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM  23
#define PCLK_GPIO_NUM  22

// ========== VARIABLES ==========
httpd_handle_t server = NULL;
SemaphoreHandle_t captureMutex = NULL;
bool flashState = false;
unsigned long lastButtonPress = 0;
bool sdAvailable = false;
int captureCount = 0;
int captureFailCount = 0;


static char htmlCache[8192];
static bool htmlCached = false;


#define JSON_BUFFER_SIZE 4096
static char jsonBuffer[JSON_BUFFER_SIZE];

// ========== UTILITAIRES ==========
bool isImageFile(const char* name) {
  String s = String(name);
  s.toLowerCase();
  bool result = s.endsWith(".jpg") || s.endsWith(".jpeg");
  return result;
}

uint32_t getNextPhotoNumber() {
  uint32_t maxNum = 0;
  File root = SD_MMC.open("/");
  if (root) {
    File file = root.openNextFile();
    while (file) {
      String name = String(file.name());
      int startIdx = name.indexOf("photo_");
      int endIdx = name.lastIndexOf(".jpg");
      if (startIdx >= 0 && endIdx > startIdx) {
        uint32_t num = name.substring(startIdx + 6, endIdx).toInt();
        if (num > maxNum) maxNum = num;
      }
      file = root.openNextFile();
    }
    root.close();
  }
  return maxNum + 1;
}

// Diagnostic mémoire avec alerte
void checkMemory() {
  int heap = ESP.getFreeHeap();
  int psram = ESP.getFreePsram();
  Serial.printf(" Heap: %u | PSRAM: %u | Captures: %d\n", heap, psram, captureCount);
  
  // Alerte si mémoire trop basse
  if (heap < 80000) {
    Serial.println(" CRITIQUE - Redémarrage dans 5s (mémoire basse)");
    delay(5000);
    ESP.restart();
  }
}

// ========== TÂCHE CAPTURE (avec stabilisation) ==========
void captureTask(void* pvParameters) {
  Serial.printf(" Capture #%d\n", captureCount + 1);
  
  // AJOUT: Stabilisation avant capture pour éviter le flou
  delay(200);
  
  // Flash
  if (flashState) {
    digitalWrite(FLASH_PIN, HIGH);
    delay(FLASH_DELAY_MS);
  }

  camera_fb_t* fb = esp_camera_fb_get();
  
  if (flashState) {
    digitalWrite(FLASH_PIN, LOW);
  }

  if (!fb) {
    captureFailCount++;
    Serial.printf(" Échec capture #%d\n", captureFailCount);
    xSemaphoreGive(captureMutex);
    vTaskDelete(NULL);
    return;
  }
  
  captureCount++;
  captureFailCount = 0;
  Serial.printf(" %u bytes (qualité=%d)\n", fb->len, JPEG_QUALITY);

  // Sauvegarde SD
  if (sdAvailable) {
    uint32_t photoNum = getNextPhotoNumber();
    char path[32];
    snprintf(path, sizeof(path), "/photo_%04u.jpg", photoNum);
    File file = SD_MMC.open(path, FILE_WRITE);
    if (file) {
      file.write(fb->buf, fb->len);
      file.close();
      Serial.printf(" Sauvegardé: %s\n", path);
    }
  }

  // Envoi TCP
  WiFiClient client;
  client.setTimeout(TCP_TIMEOUT_MS);
  if (client.connect(SERVER_HOST, SERVER_PORT)) {
    client.write(fb->buf, fb->len);
    client.flush();
    client.stop();
    Serial.printf("Envoyé à %s:%d\n", SERVER_HOST, SERVER_PORT);
  }

  esp_camera_fb_return(fb);
  Serial.println(" Capture terminée\n");
  
  xSemaphoreGive(captureMutex);
  vTaskDelete(NULL);
}

// ========== HTML OPTIMISÉ (stocké en mémoire statique) ==========
void generateHTML() {
  if (htmlCached) return;
  
  const char* html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no"/>
<title>ESP32-CAM Reconnaissance</title>
<style>
*{box-sizing:border-box}
body{font-family:Arial,Helvetica,sans-serif;background:#0a0a0a;color:#fff;text-align:center;padding:15px;margin:0}
h2{color:#0077ff;margin:0 0 15px 0;font-size:1.3rem}
.buttons{display:flex;justify-content:center;flex-wrap:wrap;gap:10px;margin-bottom:20px}
button{background:#0077ff;color:#fff;border:none;padding:12px 20px;font-size:1rem;border-radius:10px;cursor:pointer;min-width:100px;transition:0.1s}
button:active{transform:scale(0.96)}
.refresh{background:#ffaa00;color:#000}
.refresh:active{background:#ff8800}
.flash-on{background:#ff6600}
#msg{padding:8px;border-radius:8px;margin:10px auto;max-width:400px;font-size:0.85rem}
#info{position:fixed;bottom:8px;left:8px;background:#00000088;padding:4px 10px;border-radius:20px;font-size:10px;font-family:monospace}
.gallery{display:flex;flex-wrap:wrap;justify-content:center;gap:10px;margin-top:15px;max-height:55vh;overflow-y:auto;padding:10px}
.photo-card{background:#1a1a1a;border-radius:8px;padding:6px;width:110px;text-align:center}
.photo-card img{width:96px;height:96px;object-fit:cover;border-radius:6px;cursor:pointer;border:2px solid #333}
.photo-card img:hover{border-color:#0077ff}
.photo-card span{display:block;font-size:9px;color:#aaa;margin-top:4px;word-break:break-all}
</style>
</head>
<body>
<h2> ESP32-CAM <span style="font-size:12px">Reconnaissance Faciale</span></h2>

<div class="buttons">
  <button id="captureBtn" ontouchstart="capture()" onmousedown="capture()"> CAPTURER</button>
  <button id="galleryBtn" ontouchstart="loadGallery()" onmousedown="loadGallery()">GALERIE</button>
  <button id="refreshBtn" class="refresh" ontouchstart="refreshGallery()" onmousedown="refreshGallery()"> RAFRAÎCHIR</button>
  <button id="flashBtn" ontouchstart="toggleFlash()" onmousedown="toggleFlash()"> FLASH OFF</button>
</div>

<div id="msg"> Prêt - Appuyez sur CAPTURER</div>
<div id="gallery" class="gallery"></div>
<div id="info"> Prêt | 0 photo(s)</div>

<script>
let flashState = false;
let galleryCache = null;
let photoCount = 0;
let isCapturing = false;

function showMessage(txt, isError){
  let m=document.getElementById('msg');
  m.textContent=txt;
  m.style.backgroundColor=isError?'#441111':'#1a3a1a';
  m.style.color=isError?'#ff8888':'#88ff88';
  setTimeout(()=>{
    let current = document.getElementById('msg').textContent;
    if(current === txt || current === ' Photo prise ! Appuyez sur RAFRAÎCHIR'){
      document.getElementById('msg').style.backgroundColor='transparent';
      document.getElementById('msg').style.color='#aaa';
    }
  }, 2500);
}

async function capture(){
  if(isCapturing){
    showMessage(' Capture en cours...', true);
    return;
  }
  
  isCapturing=true;
  let btn=document.getElementById('captureBtn');
  let originalText = btn.textContent;
  btn.textContent='...';
  btn.disabled=true;
  showMessage(' Capture en cours...');
  document.getElementById('info').innerHTML=' Capture...';
  
  try{
    let start = Date.now();
    let r = await fetch('/capture');
    let elapsed = Date.now() - start;
    
    if(r.ok){
      showMessage(` Photo prise ! (${elapsed}ms) - Appuyez sur RAFRAÎCHIR`);
      document.getElementById('info').innerHTML=`Capture ${elapsed}ms | Appuyez sur RAFRAÎCHIR`;
      galleryCache = null;
    } else {
      showMessage(' Erreur capture', true);
      document.getElementById('info').innerHTML=' Erreur capture';
    }
  } catch(e){
    showMessage(' Erreur connexion', true);
    document.getElementById('info').innerHTML=' Erreur connexion';
  }
  
  btn.textContent=originalText;
  btn.disabled=false;
  isCapturing=false;
  
  setTimeout(()=>{
    if(document.getElementById('info').innerHTML.includes('Capture')){
      document.getElementById('info').innerHTML=` ${photoCount} photo(s) | Appuyez sur RAFRAÎCHIR`;
    }
  }, 3000);
}

async function loadGallery(){
  let gal = document.getElementById('gallery');
  
  if(galleryCache){
    gal.innerHTML = galleryCache;
    showMessage(` ${photoCount} photo(s) (cache) - Cliquez RAFRAÎCHIR`);
    document.getElementById('info').innerHTML=` ${photoCount} photo(s) (cache)`;
    return;
  }
  
  gal.innerHTML = '<div style="color:#aaa"> Chargement...</div>';
  showMessage(' Chargement...');
  document.getElementById('info').innerHTML=' Chargement...';
  
  try{
    let r = await fetch('/list');
    let photos = await r.json();
    photoCount = photos.length;
    
    if(!photos.length){
      gal.innerHTML = '<div style="color:#ffaa00"> Aucune photo - Appuyez sur CAPTURER</div>';
      showMessage(' Aucune photo trouvée', true);
      document.getElementById('info').innerHTML=' 0 photo';
      return;
    }
    
    let html = '';
    for(let p of photos){
      html += `<div class="photo-card">
                 <img src="/photo?name=${encodeURIComponent(p)}" 
                      title="${p}" 
                      loading="lazy"
                      onclick="window.open(this.src)">
                 <span>${p}</span>
               </div>`;
    }
    
    galleryCache = html;
    gal.innerHTML = html;
    showMessage(` ${photos.length} photo(s) trouvée(s)`);
    document.getElementById('info').innerHTML=` ${photos.length} photo(s)`;
    
  } catch(e){
    gal.innerHTML = '<div style="color:#f44"> Erreur lecture carte SD</div>';
    showMessage(' Erreur chargement galerie', true);
    document.getElementById('info').innerHTML=' Erreur';
  }
}

function refreshGallery(){
  showMessage(' Rafraîchissement...');
  document.getElementById('info').innerHTML=' Rafraîchissement...';
  galleryCache = null;
  loadGallery();
}

async function toggleFlash(){
  flashState = !flashState;
  let btn = document.getElementById('flashBtn');
  btn.innerHTML = flashState ? ' FLASH ON' : ' FLASH OFF';
  btn.className = flashState ? 'flash-on' : '';
  
  try{
    await fetch('/flash');
    showMessage(flashState ? ' Flash activé' : ' Flash désactivé');
  } catch(e){
    showMessage(' Erreur flash', true);
  }
}

window.onload = ()=>{
  loadGallery();
  setInterval(()=>{
    let d = new Date();
    let time = `${d.getHours().toString().padStart(2,'0')}:${d.getMinutes().toString().padStart(2,'0')}:${d.getSeconds().toString().padStart(2,'0')}`;
    let info = document.getElementById('info');
    if(!info.innerHTML.includes('Capture') && !info.innerHTML.includes('Rafraîchissement')){
      info.innerHTML = ` ${photoCount} photo(s) | ${time}`;
    }
  }, 1000);
};
</script>
</body>
</html>
)rawliteral";
  
  strncpy(htmlCache, html, sizeof(htmlCache) - 1);
  htmlCache[sizeof(htmlCache) - 1] = '\0';
  htmlCached = true;
}

// ========== HANDLERS HTTP (SANS FUITES MÉMOIRE) ==========
esp_err_t index_handler(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, htmlCache, strlen(htmlCache));
  return ESP_OK;
}

esp_err_t capture_handler(httpd_req_t* req) {
  if (xSemaphoreTake(captureMutex, 0) == pdFALSE) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Capture en cours");
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, "OK", 2);
  xTaskCreate(captureTask, "captureTask", 12288, NULL, 5, NULL);
  return ESP_OK;
}

// Handler LIST sans String (buffer statique)
esp_err_t list_handler(httpd_req_t* req) {
  httpd_resp_set_type(req, "application/json");
  
  int pos = 0;
  jsonBuffer[pos++] = '[';
  
  if (sdAvailable) {
    File root = SD_MMC.open("/");
    if (root) {
      File file = root.openNextFile();
      bool first = true;
      
      while (file && pos < JSON_BUFFER_SIZE - 100) {
        String name = String(file.name());
        if (name.startsWith("/")) name = name.substring(1);
        
        if (isImageFile(name.c_str())) {
          if (!first) {
            jsonBuffer[pos++] = ',';
          }
          first = false;
          jsonBuffer[pos++] = '"';
          
          // Copier le nom caractère par caractère
          for (int i = 0; i < name.length() && pos < JSON_BUFFER_SIZE - 10; i++) {
            char c = name[i];
            if (c == '"' || c == '\\') {
              jsonBuffer[pos++] = '\\';
            }
            jsonBuffer[pos++] = c;
          }
          jsonBuffer[pos++] = '"';
        }
        file = root.openNextFile();
      }
      root.close();
    }
  }
  
  jsonBuffer[pos++] = ']';
  jsonBuffer[pos] = '\0';
  
  httpd_resp_send(req, jsonBuffer, pos);
  return ESP_OK;
}

esp_err_t photo_handler(httpd_req_t* req) {
  char query[128];
  char param[64];

  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "name", param, sizeof(param)) != ESP_OK) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  // Construire le chemin sans utiliser String
  char fullPath[128];
  snprintf(fullPath, sizeof(fullPath), "/%s", param);
  
  File file = SD_MMC.open(fullPath, FILE_READ);
  if (!file) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
  
  uint8_t chunk[1024];
  size_t len;
  while ((len = file.read(chunk, sizeof(chunk))) > 0) {
    if (httpd_resp_send_chunk(req, (const char*)chunk, len) != ESP_OK) break;
  }
  httpd_resp_send_chunk(req, NULL, 0);
  file.close();
  return ESP_OK;
}

esp_err_t flash_handler(httpd_req_t* req) {
  flashState = !flashState;
  digitalWrite(FLASH_PIN, flashState ? HIGH : LOW);
  const char* response = flashState ? "Flash ON" : "Flash OFF";
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, response, strlen(response));
  return ESP_OK;
}

// ========== SERVEUR ==========
void startServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.stack_size = 6144;
  config.max_uri_handlers = 8;

  if (httpd_start(&server, &config) == ESP_OK) {
    httpd_uri_t uris[] = {
      {"/", HTTP_GET, index_handler, NULL},
      {"/capture", HTTP_GET, capture_handler, NULL},
      {"/list", HTTP_GET, list_handler, NULL},
      {"/photo", HTTP_GET, photo_handler, NULL},
      {"/flash", HTTP_GET, flash_handler, NULL},
    };
    for (int i = 0; i < 5; i++) {
      httpd_register_uri_handler(server, &uris[i]);
    }
    Serial.println("[OK] Serveur HTTP démarré");
  }
}

// ========== INIT CAMERA (AVEC RÉGLAGES NETTETÉ) ==========
void initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = JPEG_QUALITY;
  config.fb_count = 1;
  config.fb_location = CAMERA_FB_IN_PSRAM;

  // Cycle PWDN
  pinMode(PWDN_GPIO_NUM, OUTPUT);
  digitalWrite(PWDN_GPIO_NUM, HIGH);
  delay(100);
  digitalWrite(PWDN_GPIO_NUM, LOW);
  delay(100);

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf(" Caméra: 0x%x\n", err);
    config.fb_location = CAMERA_FB_IN_DRAM;
    err = esp_camera_init(&config);
    if (err != ESP_OK) {
      Serial.println(" Caméra HS !");
      while(1) delay(1000);
    }
  }
  

  sensor_t *s = esp_camera_sensor_get();
  
  s->set_brightness(s, 0);     
  s->set_contrast(s, 1);       
  s->set_saturation(s, 1);     
  s->set_sharpness(s, 2);      
  s->set_denoise(s, 1);        
  s->set_gain_ctrl(s, 1);      
  s->set_exposure_ctrl(s, 1);  
  s->set_awb_gain(s, 1);       
  s->set_wb_mode(s, 0);        
  
  Serial.printf("[OK] Caméra - Qualité: %d, Netteté: 2, Résolution: SVGA\n", JPEG_QUALITY);
}

// ========== SETUP ==========
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== ESP32-CAM OPTIMISÉ SANS FUITES ===\n");

  pinMode(FLASH_PIN, OUTPUT);
  digitalWrite(FLASH_PIN, LOW);
  pinMode(BOUTON_PIN, INPUT_PULLUP);

  captureMutex = xSemaphoreCreateMutex();
  
  initCamera();

  // SD
  sdAvailable = SD_MMC.begin();
  if (sdAvailable && SD_MMC.cardType() != CARD_NONE) {
    uint64_t size = SD_MMC.cardSize() / (1024 * 1024);
    Serial.printf("[OK] SD: %llu MB\n", size);
  } else {
    sdAvailable = false;
    Serial.println(" SD absente");
  }

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[OK] WiFi connecté");
    Serial.print(" IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n WiFi échoué");
  }

  generateHTML();
  startServer();
  
  Serial.println("\n PRÊT !");
  Serial.println("==================================");
  Serial.print(" http://");
  Serial.println(WiFi.localIP());
  Serial.println("==================================");
  checkMemory();
}

// ========== LOOP ==========
void loop() {
  // Bouton physique
  if (digitalRead(BOUTON_PIN) == LOW) {
    unsigned long now = millis();
    if (now - lastButtonPress > DEBOUNCE_MS) {
      lastButtonPress = now;
      Serial.println(" Bouton physique");
      if (xSemaphoreTake(captureMutex, 0) == pdTRUE) {
        xTaskCreate(captureTask, "captureTask", 12288, NULL, 5, NULL);
      }
    }
  }
  
  // Diagnostic toutes les 30 secondes
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 30000) {
    lastCheck = millis();
    checkMemory();
  }
  
  delay(10);
}
