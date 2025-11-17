// ===========================================
// CODIGO MAESTRO: PARQUEADERO INTELIGENTE V1.3 (FINAL)
// ===========================================

// --- LIBRERÍAS ---
#include <ESP32Servo.h>
#include <Wire.h> 
#include <Adafruit_GFX.h> 
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <PubSubClient.h> // Librería para MQTT

// --- CONFIGURACIÓN DE WIFI y MQTT (MODIFICAR ESTOS VALORES) ---
const char* ssid = "parqueadero";       // <<<<<<<<< ¡REEMPLAZAR SI CAMBIA!
const char* password = "parqueadero";     // <<<<<<<<< ¡REEMPLAZAR SI CAMBIA!

// MQTT Broker
const char* mqtt_server = "192.168.0.39";
const int   MQTT_PORT   = 1883;
const char* MQTT_USER   = "backend";
const char* MQTT_PASS   = "Deef3137047135$";

// Cliente WiFi y MQTT
WiFiClient espClient;
PubSubClient client(espClient);

// --- DEFINICIÓN DE PINES ---
#define PIN_SERVOMOTOR 18
#define PIN_SENSOR_ENTRADA 2      // Detección de vehículo en la entrada
#define PIN_SENSOR_SALIDA 16      // Detección de vehículo en la salida

// Definición de Pines de Cubículos (EJEMPLO: 2 cubículos)
#define NUM_CUBICULOS 2
const int PINES_CUBICULOS[NUM_CUBICULOS] = {13, 14};  
const char* NOMBRES_CUBICULOS[NUM_CUBICULOS] = {"A1", "A2"};

// Pantalla OLED I2C (SSD1306)
#define OLED_SDA 21  
#define OLED_SCL 22  
#define PANTALLA_ANCHO 128  
#define PANTALLA_ALTO 64  
#define OLED_RESET -1      

// --- CONSTANTES DE LÓGICA ---
const int ANGULO_CERRADA = 0;
const int ANGULO_ABIERTA = 90;
const int ESTADO_SENSOR_ACTIVO = LOW;  // Objeto detectado (Ocupado)
const int ESTADO_SENSOR_REPOSO = HIGH; // Sin detección (Libre)

// --- TOPICOS DE COMUNICACIÓN MQTT ---
#define TOPIC_ENTRADA_CARRO   "parqueadero/entrada/carro"  // {"cub": "A1", "estado": "Ingresando"}
#define TOPIC_SALIDA_CARRO    "parqueadero/salida/carro"   // {"estado": "Saliendo"} -> backend registra hora de salida
#define TOPIC_UBICACION_BASE  "parqueadero/ubicacion"      // Base para parqueadero/ubicacion/A1, etc.

// Estructura de datos para el Parqueadero
struct Cubiculo {
  int id;
  int pin;
  bool ocupado;
  int estadoPrevio; // Guarda el estado anterior del sensor
};
Cubiculo parqueadero[NUM_CUBICULOS];

// --- OBJETOS ---
Servo talanquera;
Adafruit_SSD1306 display(PANTALLA_ANCHO, PANTALLA_ALTO, &Wire, OLED_RESET);

// ====================================================================
// --- PROTOTIPOS DE FUNCIONES ---
// ====================================================================
void mostrarEstadoEnOLED(const char* linea1, const char* linea2);
void mostrarEstadoGeneral();
void mostrarMensajeTemporal(const char* titulo, const char* mensaje, long duracion_ms);
int  contarCubiculosLibres();
void reconnect();
void setup_wifi();
void controlarEntrada();
void controlarSalida();
void actualizarEstadoCubiculos();

// ====================================================================
// --- CONFIGURACIÓN Y LOOP ---
// ====================================================================

void setup() {
  Serial.begin(115200);

  // 1. Inicialización de Hardware
  Wire.begin(OLED_SDA, OLED_SCL);  
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {  
    Serial.println("FALLO: SSD1306 no encontrado.");
    for(;;);  
  }
  display.clearDisplay();  
  display.setTextColor(SSD1306_WHITE);  

  talanquera.attach(PIN_SERVOMOTOR);
  talanquera.write(ANGULO_CERRADA);

  // 2. Inicialización de Pines de Sensores y Cubículos
  pinMode(PIN_SENSOR_ENTRADA, INPUT_PULLUP);  
  pinMode(PIN_SENSOR_SALIDA, INPUT_PULLUP);
  for (int i = 0; i < NUM_CUBICULOS; i++) {
    parqueadero[i].id = i;  
    parqueadero[i].pin = PINES_CUBICULOS[i];
    parqueadero[i].ocupado = false;  
    parqueadero[i].estadoPrevio = ESTADO_SENSOR_REPOSO;  
    pinMode(parqueadero[i].pin, INPUT_PULLUP);
  }

  // 3. Conexión WiFi y MQTT
  setup_wifi();
  client.setServer(mqtt_server, MQTT_PORT);  

  mostrarEstadoGeneral();  
  delay(1000);
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop(); 

  // 1. Monitoreo de Cubículos (Estado Ocupado/Libre)
  actualizarEstadoCubiculos();

  // 2. Lógica de ENTRADA (Asignación)
  controlarEntrada();  

  // 3. Lógica de SALIDA (Apertura y Publicación de Salida)
  controlarSalida();

  // 4. Mostrar estado general 
  mostrarEstadoGeneral();

  delay(50);  
}

// ====================================================================
// --- FUNCIONES DE CONEXIÓN ---
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
      mostrarEstadoEnOLED("MQTT Conectado", "Listo para operar");
    } else {
      Serial.print("falló, rc=");
      Serial.print(client.state());
      Serial.println(" reintentando en 5 segundos");
      mostrarEstadoEnOLED("FALLO MQTT", "Reintentando...");
      delay(5000);
    }
  }
}

// ====================================================================
// --- FUNCIONES PRINCIPALES DE LÓGICA Y PUBLICACIÓN MQTT ---
// ====================================================================

/**
 * Controla el sensor de entrada, ASIGNA un cubículo y PUBLICA el evento.
 * Payload: {"cub": "A1", "estado": "Ingresando"}
 */
void controlarEntrada() {
  static int estadoPrevioEntrada = ESTADO_SENSOR_REPOSO;
  int estadoActualEntrada = digitalRead(PIN_SENSOR_ENTRADA);

  if (estadoActualEntrada == ESTADO_SENSOR_ACTIVO && estadoPrevioEntrada == ESTADO_SENSOR_REPOSO) {
    int indice_cubiculo_asignado = -1;
    for (int i = 0; i < NUM_CUBICULOS; i++) {
      if (digitalRead(parqueadero[i].pin) == ESTADO_SENSOR_REPOSO) { // Busca el libre
        indice_cubiculo_asignado = i;
        break; 
      }
    }

    if (indice_cubiculo_asignado != -1) {
      const char* nombre_asignado = NOMBRES_CUBICULOS[indice_cubiculo_asignado];
      
      mostrarMensajeTemporal("ASIGNADO:", nombre_asignado, 3000); 

      // Publicar el evento de ingreso CON la asignación
      String payload_str = "{\"cub\": \"";
      payload_str += nombre_asignado;
      payload_str += "\", \"estado\": \"Ingresando\"}";
      
      if (client.publish(TOPIC_ENTRADA_CARRO, payload_str.c_str())) {
        Serial.printf("EVENTO: Ingreso publicado. Cubículo ASIGNADO: %s\n", nombre_asignado);
        talanquera.write(ANGULO_ABIERTA);
      } else {
        Serial.println("FALLO al publicar entrada.");
        mostrarMensajeTemporal("FALLO MQTT", "Revisa conexión", 3000);
      }
    } else {
      Serial.println("FALLO: Parqueadero lleno.");
      mostrarMensajeTemporal("PARQUEADERO LLENO", "Espere por favor", 5000);
    }
  }

  // Lógica de cierre de talanquera después de pasar
  if (talanquera.read() == ANGULO_ABIERTA && 
      estadoActualEntrada == ESTADO_SENSOR_REPOSO && 
      digitalRead(PIN_SENSOR_SALIDA) == ESTADO_SENSOR_REPOSO) {

    Serial.println("Vehículo ha pasado. Cerrando entrada...");
    mostrarMensajeTemporal("ENTRADA", "CERRANDO...", 2000);
    talanquera.write(ANGULO_CERRADA);
  }
  
  estadoPrevioEntrada = estadoActualEntrada;
}


/**
 * Monitorea los sensores de los cubículos y PUBLICA el estado.
 * El backend usa este evento para cambiar el estado de OCUPADO a LIBRE
 * y disparar el cálculo final del cobro.
 * Payload: {"cubiculo": "A1", "estado": "Ocupado"} o {"cubiculo": "A1", "estado": "Libre"}
 */
void actualizarEstadoCubiculos() {
  for (int i = 0; i < NUM_CUBICULOS; i++) {
    int estadoActual = digitalRead(parqueadero[i].pin);
    
    if (estadoActual != parqueadero[i].estadoPrevio) {
      
      const char* nombre_cubiculo = NOMBRES_CUBICULOS[i];
      String topic_str = String(TOPIC_UBICACION_BASE) + "/" + nombre_cubiculo;
      String payload_str;
      
      if (estadoActual == ESTADO_SENSOR_ACTIVO) {
        parqueadero[i].ocupado = true;
        // Ocupado
        payload_str = "{\"cubiculo\": \"";
        payload_str += nombre_cubiculo;
        payload_str += "\", \"estado\": \"Ocupado\"}"; 
        Serial.printf("EVENTO: Cubículo %s OCUPADO. Publicando...\n", nombre_cubiculo);
      } else {
        parqueadero[i].ocupado = false;
        // Libre
        payload_str = "{\"cubiculo\": \"";
        payload_str += nombre_cubiculo;
        payload_str += "\", \"estado\": \"Libre\"}"; 
        Serial.printf("EVENTO: Cubículo %s LIBRE. Publicando...\n", nombre_cubiculo);
      }
      
      if (client.publish(topic_str.c_str(), payload_str.c_str())) {
        Serial.println("Publicación de UBICACIÓN exitosa.");
      } else {
        Serial.println("FALLO al publicar ubicación.");
      }
      
      parqueadero[i].estadoPrevio = estadoActual;
    }
  }
}

/**
 * Controla el sensor de salida. PUBLICA un evento para que el backend
 * registre la hora de salida.
 * Payload: {"estado": "Saliendo"}
 */
void controlarSalida() {
  static int estadoPrevioSalida = ESTADO_SENSOR_REPOSO;
  int estadoActualSalida = digitalRead(PIN_SENSOR_SALIDA);
  
  // Detección de vehículo en la SALIDA
  if (estadoActualSalida == ESTADO_SENSOR_ACTIVO && 
      estadoPrevioSalida == ESTADO_SENSOR_REPOSO) {
    
    // Abrir talanquera si está cerrada
    if (talanquera.read() == ANGULO_CERRADA) {
      Serial.println("Vehículo en SALIDA. ABRIENDO TALANQUERA.");
      mostrarMensajeTemporal("SALIDA", "ABRIENDO...", 3000);
      talanquera.write(ANGULO_ABIERTA);

      // PUBLICAR EVENTO DE SALIDA (Backend registra la hora)
      const char* payload = "{\"estado\": \"Saliendo\"}";
      if (!client.publish(TOPIC_SALIDA_CARRO, payload)) {
        Serial.println("FALLO al publicar evento de SALIDA.");
      }
    }
    
  } else if (estadoActualSalida == ESTADO_SENSOR_REPOSO && 
              estadoPrevioSalida == ESTADO_SENSOR_ACTIVO && 
              talanquera.read() == ANGULO_ABIERTA) {
    // El vehículo ha pasado, cerrar talanquera
    Serial.println("Vehículo ha salido. Cerrando salida...");
    mostrarMensajeTemporal("SALIDA", "GRACIAS POR SU VISITA", 3000);
    talanquera.write(ANGULO_CERRADA);
  }
  
  estadoPrevioSalida = estadoActualSalida;
}

int contarCubiculosLibres() {
  int count = 0;
  for (int i = 0; i < NUM_CUBICULOS; i++) {
    if (digitalRead(parqueadero[i].pin) == ESTADO_SENSOR_REPOSO) {
      count++;
    }
  }
  return count;
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

  int libres = contarCubiculosLibres();

  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("PARQUEADERO INTELIGENTE");
  display.drawFastHLine(0, 10, PANTALLA_ANCHO, SSD1306_WHITE);  

  display.setTextSize(2);
  display.setCursor(0, 15);
  display.print("LIBRES: ");
  display.println(libres);

  display.setTextSize(1);
  int y_start = 40;
  for (int i = 0; i < NUM_CUBICULOS; i++) {
    display.setCursor(0, y_start + (i * 10));
    display.print("CUB ");
    display.print(NOMBRES_CUBICULOS[i]);
    display.print(": ");
    display.print(digitalRead(parqueadero[i].pin) == ESTADO_SENSOR_ACTIVO ? "OCUPADO" : "LIBRE");
  }

  display.display();
}

void mostrarMensajeTemporal(const char* titulo, const char* mensaje, long duracion_ms) {
  display.clearDisplay();
  display.setTextSize(2);  
  display.setCursor(0, 0);
  display.println(titulo);

  display.setTextSize(2); 
  display.setCursor(0, 25);
  display.println(mensaje);

  display.display();
  delay(duracion_ms); 
}