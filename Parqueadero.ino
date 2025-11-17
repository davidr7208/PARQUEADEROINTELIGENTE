// ===========================================
// CODIGO MAESTRO: PARQUEADERO INTELIGENTE V1.23 (BAJA SENSIBILIDAD)
// ===========================================

// --- LIBRERÍAS ---
// --- david h ---
#include <ESP32Servo.h>
#include <Wire.h> 
#include <Adafruit_GFX.h> 
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h> 

// --- CONFIGURACIÓN DE WIFI y MQTT ---
const char* ssid = "parqueadero"; 
const char* password = "parqueadero"; 
const char* mqtt_server = "192.168.0.100"; // <<--- VERIFICA ESTA IP
const int   MQTT_PORT   = 1883;
const char* MQTT_USER   = "parqueadero";
const char* MQTT_PASS   = "parqueadero";

WiFiClient espClient;
PubSubClient client(espClient);

// --- DEFINICIÓN DE PINES GENERALES y CUBÍCULOS ---
#define PIN_SERVOMOTOR 18
#define PIN_SENSOR_ENTRADA 2      
#define PIN_SENSOR_SALIDA 16      
#define NUM_CUBICULOS 2
const char* NOMBRES_CUBICULOS[NUM_CUBICULOS] = {"A1", "A2"};
const int PINES_CUBICULOS[NUM_CUBICULOS] = {13, 14}; 

// Pantalla OLED I2C (SSD1306)
#define OLED_SDA 21  
#define OLED_SCL 22  
#define PANTALLA_ANCHO 128  
#define PANTALLA_ALTO 64  
#define OLED_RESET -1       

// --- CONSTANTES DE LÓGICA ---
const int ANGULO_CERRADA = 0;
const int ANGULO_ABIERTA = 90;
const int ESTADO_SENSOR_ACTIVO = LOW;  
const int ESTADO_SENSOR_REPOSO = HIGH; 
const long DEBOUNCE_DELAY_MS = 10000; // 10s Bloqueo de asignación (cooldown de tiempo)
const long CUP_LLENO_TIMEOUT_MS = 5000; 
const long CUBICULO_DEBOUNCE_MS = 10000; // 10s Anti-rebote para Sensores de CUBÍCULO
const long DISPLAY_MESSAGE_DURATION_MS = 5000; // 5 segundos para mensajes temporales
const long TIEMPO_CIERRE_AUTOMATICO_MS = 5000; // 5 segundos para cerrar la talanquera (TIMEOUT)
const int FILTRO_SENSIBILIDAD_MS = 100; // 100ms para ignorar ruido en el sensor de entrada

// --- TOPICOS DE COMUNICACIÓN MQTT ---
#define TOPIC_ENTRADA_CARRO          "parqueadero/entrada/carro"     
#define TOPIC_SALIDA_CARRO           "parqueadero/salida/carro"      
#define TOPIC_UBICACION_BASE         "parqueadero/ubicacion"         
#define TOPIC_CONTROL_TALANQUERA     "parqueadero/control/talanquera"
#define TOPIC_DISPLAY_ESTADO_GENERAL "parqueadero/display/estado_general" 

// --- VARIABLES DE ESTADO GLOBALES ---
volatile int estado_talanquera_logico = 0; 
int estadoPrevioEntrada = ESTADO_SENSOR_REPOSO; 
int estadoPrevioSalida = ESTADO_SENSOR_REPOSO; 
long last_publish_time_entrada = 0; 
bool cupo_lleno_activo = false; 
long cupo_lleno_timestamp = 0; 
long ultima_actualizacion_display = 0;
bool sistema_iniciado = false; 
long last_cubiculo_publish_time[NUM_CUBICULOS] = {0}; 
volatile bool asignacion_activa = false; 
volatile bool mensaje_temporal_activo = false;
volatile long mensaje_temporal_timestamp = 0; 

// Variables para el cierre automático
volatile bool talanquera_abierta_por_tiempo = false;
volatile long timestamp_apertura = 0; 

// Estructuras y Objetos
struct Cubiculo {
  int id;
  int pin;  
  bool ocupado;
  int estadoPrevio; 
};
Cubiculo parqueadero[NUM_CUBICULOS];
struct EstadoDB {
  String nombre;
  String estado; 
};
EstadoDB estados_db[NUM_CUBICULOS];
int libres_totales_db = NUM_CUBICULOS; 
Servo talanquera;
Adafruit_SSD1306 display(PANTALLA_ANCHO, PANTALLA_ALTO, &Wire, OLED_RESET);

// --- PROTOTIPOS DE FUNCIONES ---
void callback(char* topic, byte* payload, unsigned int length);
void mostrarEstadoEnOLED(const char* linea1, const char* linea2);
void mostrarEstadoGeneral();
void mostrarMensajeTemporal(const char* titulo, const char* mensaje, long duracion_ms);
void reconnect();
void setup_wifi();
void controlarEntrada();
void controlarSalida();
void actualizarEstadoCubiculos(); 
void cerrar_talanquera_entrada(); 

// ====================================================================
// --- CONFIGURACIÓN Y LOOP ---
// ====================================================================

void setup() {
  Serial.begin(115200);

  for (int i = 0; i < NUM_CUBICULOS; i++) {
    estados_db[i].nombre = NOMBRES_CUBICULOS[i];
    estados_db[i].estado = "Libre"; 
  }
  
  Wire.begin(OLED_SDA, OLED_SCL);  
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {  
    Serial.println("FALLO: SSD1306 no encontrado.");
    for(;;);  
  }
  display.clearDisplay();  
  display.setTextColor(SSD1306_WHITE);  

  talanquera.attach(PIN_SERVOMOTOR);
  talanquera.write(ANGULO_CERRADA); 

  pinMode(PIN_SENSOR_ENTRADA, INPUT_PULLUP);  
  pinMode(PIN_SENSOR_SALIDA, INPUT_PULLUP);
  
  estadoPrevioEntrada = digitalRead(PIN_SENSOR_ENTRADA);
  estadoPrevioSalida = digitalRead(PIN_SENSOR_SALIDA);
  
  for (int i = 0; i < NUM_CUBICULOS; i++) {
    parqueadero[i].id = i;  
    parqueadero[i].pin = PINES_CUBICULOS[i]; 
    parqueadero[i].ocupado = false;  
    parqueadero[i].estadoPrevio = ESTADO_SENSOR_REPOSO; 
    pinMode(parqueadero[i].pin, INPUT_PULLUP); 
    last_cubiculo_publish_time[i] = 0; 
  }

  setup_wifi();
  client.setServer(mqtt_server, MQTT_PORT);  
  client.setCallback(callback); 

  mostrarEstadoGeneral();  
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop(); 
  
  long current_time = millis();
  
  // 1. Lógica de Cupo Lleno
  if (cupo_lleno_activo && current_time - cupo_lleno_timestamp > CUP_LLENO_TIMEOUT_MS) {
      cupo_lleno_activo = false; 
  }
  
  // 2. Lógica de Cierre Automático (5 segundos de timeout)
  if (talanquera_abierta_por_tiempo && current_time - timestamp_apertura >= TIEMPO_CIERRE_AUTOMATICO_MS) {
      
      Serial.println("INFO: Cierre automático por tiempo expirado (5s).");
      
      talanquera.attach(PIN_SERVOMOTOR); 
      talanquera.write(ANGULO_CERRADA);
      
      talanquera_abierta_por_tiempo = false;
      estado_talanquera_logico = 0; 
  }
  
  // 3. Lógica de Tiempo de Mensaje Temporal (5 segundos)
  if (mensaje_temporal_activo) {
      if (current_time - mensaje_temporal_timestamp >= DISPLAY_MESSAGE_DURATION_MS) {
          mensaje_temporal_activo = false; 
          mostrarEstadoGeneral();
      }
  }

  if (sistema_iniciado) {
    actualizarEstadoCubiculos();
  }

  controlarEntrada();  
  controlarSalida();
  cerrar_talanquera_entrada(); // Cierre por sensor (si el vehículo pasa antes del timeout)

  // 4. Actualización del Display
  if (!cupo_lleno_activo && !mensaje_temporal_activo) {
    if (current_time - ultima_actualizacion_display > 500) { 
      mostrarEstadoGeneral();
      ultima_actualizacion_display = current_time;
    }
  }

  delay(50);   
}

// ====================================================================
// --- FUNCIONES DE CONEXIÓN Y MQTT ---
// ====================================================================

void setup_wifi() {
  mostrarEstadoEnOLED("Conectando", "a WiFi...");
  WiFi.begin(ssid, password);
  Serial.print("Conectando a WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Conectado!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Intentando conexión MQTT...");
    String clientId = "ESP32Client-";
    clientId += String(random(0xffff), HEX);

    if (client.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
      Serial.println("conectado.");
      client.subscribe(TOPIC_CONTROL_TALANQUERA); 
      client.subscribe(TOPIC_DISPLAY_ESTADO_GENERAL);
      mostrarEstadoEnOLED("MQTT Conectado", "Listo para operar");
    } else {
      Serial.print("falló, rc=");
      Serial.print(client.state());
      Serial.println(" reintentando en 5 segundos");
      mostrarEstadoEnOLED("FALLO MQTT", "Reintentando...");
      delay(5000);
    }
    
    sistema_iniciado = false; 
  }
}

void callback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<256> doc;  
  DeserializationError error = deserializeJson(doc, payload, length);

  if (error) {
    Serial.print(F("Fallo de JSON en callback: "));
    Serial.println(error.f_str());
    return; 
  }
  
  // 1. Lógica de CONTROL (Apertura de Talanquera)
  if (String(topic) == TOPIC_CONTROL_TALANQUERA) {
    const char* orden = doc["orden"];
    const char* cubiculo_asignado = doc["cub"];

    if (orden != NULL && strcmp(orden, "ABRIR") == 0) { 
      talanquera.attach(PIN_SERVOMOTOR); 
      talanquera.write(ANGULO_ABIERTA); 
      estado_talanquera_logico = 1; 
      Serial.print("ORDEN EJECUTADA: Abriendo talanquera. Cubículo: ");
      Serial.println(cubiculo_asignado);
      
      // Activar temporizador de cierre automático de 5s
      talanquera_abierta_por_tiempo = true;
      timestamp_apertura = millis();
      
      mostrarMensajeTemporal("ASIGNADO:", cubiculo_asignado, DISPLAY_MESSAGE_DURATION_MS); 
    }
  }

  // 2. Lógica para ACTUALIZAR EL DISPLAY (Desde la DB)
  if (String(topic) == TOPIC_DISPLAY_ESTADO_GENERAL) {
    
    if (libres_totales_db == 0 && doc["libres"].as<int>() > 0) {
        cupo_lleno_activo = false; 
    }
    
    libres_totales_db = doc["libres"].as<int>(); 

    JsonArray data = doc["data"].as<JsonArray>();

    for (int i = 0; i < NUM_CUBICULOS; i++) {
      if (data.size() > i) { 
        estados_db[i].nombre = data[i]["cub"].as<String>();
        estados_db[i].estado = data[i]["est"].as<String>();
      }
    }
    
    if (!sistema_iniciado) {
        sistema_iniciado = true;
        Serial.println("SISTEMA INICIADO: Primer estado de DB recibido. Sensores activados.");
    }
  }
}

// ====================================================================
// --- FUNCIONES DE LÓGICA PRINCIPAL ---
// ====================================================================

void controlarEntrada() {
  int estadoActualEntrada = digitalRead(PIN_SENSOR_ENTRADA);
  long current_time = millis(); 

  // --- 1. Lógica de Activación (Flanco de bajada: Reposo -> Activo) ---
  if (estadoActualEntrada == ESTADO_SENSOR_ACTIVO && estadoPrevioEntrada == ESTADO_SENSOR_REPOSO) {
    
    // >>> FILTRO DE SENSIBILIDAD POR SOFTWARE (100ms) <<<
    delay(FILTRO_SENSIBILIDAD_MS); 
    estadoActualEntrada = digitalRead(PIN_SENSOR_ENTRADA);
    if (estadoActualEntrada == ESTADO_SENSOR_REPOSO) {
        // Si regresó a reposo en 100ms, fue ruido o rebote, lo ignoramos.
        Serial.println("AVISO: Detección ignorada. Duración menor a 100ms (Filtro de Sensibilidad).");
        estadoPrevioEntrada = ESTADO_SENSOR_REPOSO; 
        return;
    }
    // >>> FIN FILTRO <<<
    
    // BLOQUEO 1: Bloquea si ya hubo una asignación activa en este ciclo de detección (el objeto sigue delante).
    if (asignacion_activa) {
        Serial.println("AVISO: Evento ignorado. Asignación ya en curso o bloqueo activo.");
        estadoPrevioEntrada = estadoActualEntrada;
        return;
    }

    // BLOQUEO 2: Bloquea si no ha pasado el tiempo de cooldown (10s) desde la última asignación.
    if (current_time - last_publish_time_entrada < DEBOUNCE_DELAY_MS) {
        Serial.println("AVISO: Evento de entrada ignorado por BLOQUEO DE 10s (cooldown).");
        estadoPrevioEntrada = estadoActualEntrada;
        return; 
    }
    
    // Si pasa los dos bloqueos, se considera un nuevo vehículo:
    last_publish_time_entrada = current_time; 

    if (libres_totales_db <= 0) {
        cupo_lleno_activo = true; 
        cupo_lleno_timestamp = current_time;
        mostrarMensajeTemporal("AVISO:", "CUPO LLENO", DISPLAY_MESSAGE_DURATION_MS); 
        Serial.println("AVISO: Cupo lleno, no se envía orden al backend.");
        estadoPrevioEntrada = estadoActualEntrada; 
        return; 
    }
    
    if (talanquera.read() == ANGULO_ABIERTA) {
        Serial.println("AVISO: Sensor de entrada activo, pero talanquera ya está abierta.");
        estadoPrevioEntrada = estadoActualEntrada; 
        return;
    }

    // PUBLICACIÓN ÚNICA DEL EVENTO
    const char* payload = "{\"estado\": \"Esperando\"}";
    
    if (client.publish(TOPIC_ENTRADA_CARRO, payload)) {
      Serial.println("EVENTO: Entrada de vehículo notificada al backend.");
      mostrarMensajeTemporal("EN ESPERA", "Asignando Cubículo...", DISPLAY_MESSAGE_DURATION_MS); 
      asignacion_activa = true; // Activa el bloqueo hasta que el sensor pase a REPOSO
    } else {
      Serial.println("FALLO al publicar entrada.");
      mostrarMensajeTemporal("FALLO MQTT", "Revisa conexión", DISPLAY_MESSAGE_DURATION_MS);
    }
  }

  // --- 2. Lógica de Reset (Restablece la asignacion_activa cuando el sensor pasa a REPOSO) ---
  if (estadoActualEntrada == ESTADO_SENSOR_REPOSO && estadoPrevioEntrada == ESTADO_SENSOR_ACTIVO) {
      Serial.println("INFO: Sensor de Entrada liberado. Listo para nueva detección.");
      asignacion_activa = false; 
  }
  
  estadoPrevioEntrada = estadoActualEntrada; 
}

void cerrar_talanquera_entrada() {
  int estadoActualEntrada = digitalRead(PIN_SENSOR_ENTRADA);
  
  // Condición de cierre: Talanquera Abierta Y Sensor en Reposo
  if (estado_talanquera_logico == 1 && // Abierta por Entrada
      talanquera.read() == ANGULO_ABIERTA && 
      estadoActualEntrada == ESTADO_SENSOR_REPOSO) { 

    Serial.println("Vehículo ha pasado por entrada. Cerrando talanquera...");
    mostrarMensajeTemporal("ENTRADA", "CERRANDO...", 2000);
    talanquera.attach(PIN_SERVOMOTOR); 
    talanquera.write(ANGULO_CERRADA);
    estado_talanquera_logico = 0; 
    talanquera_abierta_por_tiempo = false; // Desactiva el timeout si el cierre fue por sensor
  }
}


/**
 * SOLO PERMITE LA TRANSICIÓN PENDIENTE/ASIGNADO A OCUPADO.
 */
void actualizarEstadoCubiculos() {
  long current_time = millis();
  
  for (int i = 0; i < NUM_CUBICULOS; i++) {
    
    // CONDICIÓN DE ANTI-REBOTE (10 segundos)
    if (current_time - last_cubiculo_publish_time[i] < CUBICULO_DEBOUNCE_MS) {
        continue;
    }

    int estadoActual = digitalRead(parqueadero[i].pin);  
    
    if (estadoActual != parqueadero[i].estadoPrevio) {
      
      delay(50);
      estadoActual = digitalRead(parqueadero[i].pin);
      if (estadoActual == parqueadero[i].estadoPrevio) continue; 
      
      const char* nombre_cubiculo = NOMBRES_CUBICULOS[i];
      String topic_str = String(TOPIC_UBICACION_BASE) + "/" + nombre_cubiculo;
      
      String estado_db_actual = estados_db[i].estado; 
      
      if (estadoActual == ESTADO_SENSOR_ACTIVO) {
        // TRANSICIÓN DESEADA: Asignado/Pendiente -> OCUPADO
        if (estado_db_actual == "Pendiente" || estado_db_actual == "Asignado") {
            String payload_str = "{\"cubiculo\": \"";
            payload_str += nombre_cubiculo;
            payload_str += "\", \"estado\": \"Ocupado\"}"; 

            client.publish(topic_str.c_str(), payload_str.c_str());
            Serial.printf("EVENTO: Cubículo %s OCUPADO (Confirmado). Desde estado: %s\n", nombre_cubiculo, estado_db_actual.c_str());
            last_cubiculo_publish_time[i] = current_time; // Reinicia el contador de 10s
        }
        
      } else { 
        // IGNORAMOS ESTADO_SENSOR_REPOSO (Para forzar cobro manual)
        Serial.printf("AVISO: Cubículo %s en Reposo. Evento 'Libre' IGNORADO por diseño.\n", nombre_cubiculo);
      }
      
      parqueadero[i].estadoPrevio = estadoActual;
    }
  }
}

void controlarSalida() {
  int estadoActualSalida = digitalRead(PIN_SENSOR_SALIDA);
  
  if (estadoActualSalida == ESTADO_SENSOR_ACTIVO && 
      estadoPrevioSalida == ESTADO_SENSOR_REPOSO) {
    
    if (talanquera.read() == ANGULO_CERRADA) {
      mostrarMensajeTemporal("SALIDA", "ABRIENDO...", 3000);
      talanquera.attach(PIN_SERVOMOTOR); 
      talanquera.write(ANGULO_ABIERTA);
      estado_talanquera_logico = 2; 

      const char* payload = "{\"estado\": \"Saliendo\"}";
      client.publish(TOPIC_SALIDA_CARRO, payload);
      Serial.println("EVENTO: Salida de vehículo notificada.");
    }
    
  } else if (estadoActualSalida == ESTADO_SENSOR_REPOSO && 
              estadoPrevioSalida == ESTADO_SENSOR_ACTIVO && 
              talanquera.read() == ANGULO_ABIERTA &&
              estado_talanquera_logico == 2) { 
    
    mostrarMensajeTemporal("SALIDA", "GRACIAS POR SU VISITA", 3000);
    talanquera.attach(PIN_SERVOMOTOR); 
    talanquera.write(ANGULO_CERRADA);
    estado_talanquera_logico = 0; 
  }
  
  estadoPrevioSalida = estadoActualSalida; 
}

// ====================================================================
// --- FUNCIONES DE CONTROL DE DISPLAY OLED ---
// ====================================================================

void mostrarEstadoEnOLED(const char* linea1, const char* linea2) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(linea1);
  display.setCursor(0, 15);
  display.println(linea2);
  display.display();
}

void mostrarEstadoGeneral() {
  if (!client.connected()) return;

  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("PARQUEADERO INTELIGEN");
  display.drawFastHLine(0, 10, PANTALLA_ANCHO, SSD1306_WHITE);  

  display.setTextSize(2);
  display.setCursor(0, 15);
  display.print("LIBRES: ");
  
  if (cupo_lleno_activo) {
      display.println("LLENO"); 
  } else {
      display.println(libres_totales_db); 
  }

  display.setTextSize(1);
  int y_start = 40;
  for (int i = 0; i < NUM_CUBICULOS; i++) {
    
    String estado_db = estados_db[i].estado; 
    
    display.setCursor(0, y_start + (i * 10));
    display.print("CUB ");
    display.print(estados_db[i].nombre);
    display.print(": ");
    
    if (estado_db == "Ocupado") {
      display.print("OCUPADO");
    } else if (estado_db == "Pendiente") {
      display.print("ASIGNADO"); 
    } else { // Libre
      display.print("LIBRE");
    }
  }

  display.display();
}

void mostrarMensajeTemporal(const char* titulo, const char* mensaje, long duracion_ms) {
  // Dibuja el mensaje
  display.clearDisplay();
  display.setTextSize(2);  
  display.setCursor(0, 0);
  display.println(titulo);

  display.setTextSize(2); 
  display.setCursor(0, 25);
  display.println(mensaje);

  display.display();
  
  // Inicia el temporizador de 5 segundos
  mensaje_temporal_activo = true;
  mensaje_temporal_timestamp = millis();
}
