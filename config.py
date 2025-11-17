# config.py

# --- CONFIGURACIÓN DE LA BASE DE DATOS MYSQL ---
MYSQL_HOST = 'localhost'
MYSQL_USER = 'root'
MYSQL_PASSWORD = '' 
MYSQL_DB = 'parqueadero_db'
MYSQL_PORT = 3306

# --- CONFIGURACIÓN DE MQTT BROKER (Mosquitto) ---
MQTT_BROKER = 'localhost' 
MQTT_PORT = 1883
MQTT_USER = 'parqueadero'
MQTT_PASSWORD = 'parqueadero'
# TÓPICOS DE ENTRADA (ESP32 publica evento de 'Esperando')
MQTT_TOPIC_ENTRADA_CARRO = "parqueadero/entrada/carro" 
#  CORRECCIÓN: Tópico de salida debe coincidir con el ESP32
TOPIC_SALIDA_CARRO = "parqueadero/salida/carro" 
MQTT_TOPIC_ENTRADA_MOTO = "parqueadero/entrada/moto" 

# Tópico para que el backend ordene al ESP32 qué hacer (abrir talanquera)
TOPIC_CONTROL_TALANQUERA = "parqueadero/control/talanquera" 

# Tópico para que los sensores internos reporten ocupación/liberación (ej. parqueadero/ubicacion/A1)
MQTT_TOPIC_UBICACION = "parqueadero/ubicacion" 

# TÓPICO CORREGIDO: Tópico para que el Backend envíe el estado general al Display (OLED)
TOPIC_DISPLAY_ESTADO_GENERAL = "parqueadero/display/estado_general"

# Tópico de visualización (no usado en la lógica central, pero mantenido)
MQTT_TOPIC_ASIGNACION_DISPLAY = "parqueadero/asignacion/display" 
# -------------------------------------------------------------------------------

# --- TIPOS DE VEHÍCULO PARA TARIFAS (Usados como claves en la DB) ---
TIPO_CARRO = 'CARRO'
TIPO_MOTO = 'MOTO'

# --- CONSTANTES DE NEGOCIO ---
# El cubículo 'Asignado'/'Pendiente' caduca y se cobra 0 si el tiempo total es menor o igual a este valor.
TIEMPO_GRACIA_MINUTOS = 0 
# --- CONFIGURACIÓN DE LIMPIEZA AUTOMÁTICA ---
# Frecuencia con la que se revisarán las reservas pendientes caducadas.
INTERVALO_LIMPIEZA_SEGUNDOS = 90