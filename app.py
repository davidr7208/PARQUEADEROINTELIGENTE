# ===========================================
# CODIGO MAESTRO: PARQUEADERO INTELIGENTE - BACKEND FINAL (CODIGO UNICO Y COBRO MANUAL)
# ===========================================
from flask import Flask, render_template, jsonify, request
from flask_mysqldb import MySQL
from datetime import datetime, timedelta
from config import *
from math import ceil 
import time
from MySQLdb import cursors
import logging
import sys
from flask_apscheduler import APScheduler 
import paho.mqtt.client as mqtt
import json

# Configuración básica de logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s',
    handlers=[
        logging.StreamHandler(sys.stdout)
    ]
)
logger = logging.getLogger('FlaskApp')

app = Flask(__name__)

# Configuración de la DB
app.config['MYSQL_HOST'] = MYSQL_HOST
app.config['MYSQL_USER'] = MYSQL_USER
app.config['MYSQL_PASSWORD'] = MYSQL_PASSWORD
app.config['MYSQL_DB'] = MYSQL_DB
app.config['MYSQL_PORT'] = MYSQL_PORT
mysql = MySQL(app)

# Inicializar Scheduler (Configuración)
scheduler = APScheduler()
scheduler.init_app(app)

# Inicialización del cliente MQTT
client_mqtt = mqtt.Client(client_id="FlaskBackend", clean_session=True) 

# ------------------------- FUNCIONES DE CONEXIÓN Y MQTT CALLBACKS -------------------------

def on_connect(client, userdata, flags, rc):
    """Función de callback que se ejecuta al conectar al broker MQTT."""
    logger.info("MQTT: Conectado al broker con resultado: " + str(rc))
    client.subscribe(MQTT_TOPIC_ENTRADA_CARRO) 
    client.subscribe(f"{MQTT_TOPIC_UBICACION}/#") 
    client.subscribe(TOPIC_SALIDA_CARRO) 
    logger.info(f"MQTT: Suscrito a {MQTT_TOPIC_ENTRADA_CARRO}, {MQTT_TOPIC_UBICACION}/# y {TOPIC_SALIDA_CARRO}.")

def on_message(client, userdata, msg):
    """Función de callback que procesa los mensajes MQTT del ESP32."""
    with app.app_context(): 
        try:
            payload = json.loads(msg.payload.decode('utf-8'))
            logger.info(f"MQTT: Mensaje recibido en {msg.topic}. Payload: {payload}")

            # 1. LÓGICA DE ASIGNACIÓN (Entrada del vehículo - Pin 2 Carros)
            if msg.topic == MQTT_TOPIC_ENTRADA_CARRO:
                if payload.get("estado") == "Esperando":
                    asignar_cubiculo_y_ordenar_apertura(client) 
                
            # 2. LÓGICA DE CUBÍCULOS (Reporte de Ocupación/Liberación)
            elif msg.topic.startswith(MQTT_TOPIC_UBICACION):
                manejar_reporte_cubiculo(msg.topic, payload)

            # 3. LÓGICA DE SALIDA 
            elif msg.topic == TOPIC_SALIDA_CARRO:
                logger.info("EVENTO: Vehículo detectado en la salida. Trazabilidad guardada.")

        except json.JSONDecodeError:
            logger.error(f"MQTT: Error al decodificar JSON del tópico {msg.topic}.")
        except Exception as e:
            logger.error(f"MQTT: Error general al procesar mensaje en {msg.topic}: {e}")

def asignar_cubiculo_y_ordenar_apertura(client_mqtt):
    """
    Busca el primer cubículo 'Libre' para CARROS ('A%'), registra el ingreso como 'Pendiente',
    genera un código único basado en el registro_id, y envía la orden de apertura.
    """
    db = mysql.connection
    cur = db.cursor()
    
    sql_select = "SELECT id, nombre, tipo_vehiculo FROM cubiculos WHERE estado = 'Libre' AND nombre LIKE 'A%' ORDER BY nombre ASC LIMIT 1"
    
    try:
        cur.execute(sql_select)
        resultado = cur.fetchone()

        if resultado:
            cubiculo_id, cubiculo_nombre, tipo_vehiculo_default = resultado
            
            # 1. Registrar el cobro con PLACA TEMPORAL para obtener el ID de registro
            sql_insert_registro = "INSERT INTO registro_cobro (hora_ingreso, cubiculo_id, placa) VALUES (%s, %s, %s)"
            # Usamos 'TEMP' para reservar el registro y obtener el ID
            cur.execute(sql_insert_registro, (datetime.now(), cubiculo_id, 'TEMP')) 
            registro_cobro_id = cur.lastrowid

            # >>> GENERACIÓN DEL CÓDIGO ÚNICO (Letra A + ID del registro con relleno) <<<
            codigo_unico = f"A-{registro_cobro_id:03d}" 
            
            # 2. Actualizar el registro de cobro y el cubículo con el CÓDIGO ÚNICO
            sql_update_registro = "UPDATE registro_cobro SET placa = %s WHERE id = %s"
            cur.execute(sql_update_registro, (codigo_unico, registro_cobro_id))

            sql_update_cubiculo = "UPDATE cubiculos SET estado = 'Pendiente', timestamp_ultima_actualizacion = %s, registro_cobro_id = %s, placa = %s, tipo_vehiculo = %s WHERE id = %s"
            cur.execute(sql_update_cubiculo, (datetime.now(), registro_cobro_id, codigo_unico, tipo_vehiculo_default, cubiculo_id))

            db.commit()
            
            # 3. Publicar la orden
            payload_orden = json.dumps({"orden": "ABRIR", "cub": cubiculo_nombre})
            client_mqtt.publish(TOPIC_CONTROL_TALANQUERA, payload_orden, qos=1) 
            
            logger.info(f"ASIGNACIÓN EXITOSA: Cubículo {cubiculo_nombre} asignado (Código: {codigo_unico}). Orden de apertura enviada.")
            
        else:
            logger.warning("ASIGNACIÓN FALLIDA: Cupo Lleno (No hay cubículos 'A' disponibles).")

    except Exception as e:
        db.rollback()
        logger.error(f"ERROR al asignar cubículo: {e}")
    finally:
        cur.close()

def manejar_reporte_cubiculo(topic, payload):
    """
    Maneja el reporte de Ocupado. La liberación y cobro AUTOMÁTICO
    por sensor ha sido ELIMINADA para cobro manual estricto.
    """
    cubiculo_nombre = topic.split('/')[-1] 
    estado_sensor = payload.get("estado")
    
    db = mysql.connection
    cur = db.cursor()

    try:
        if estado_sensor == 'Ocupado':
            # Solo confirmamos la ocupación (Pendiente -> Ocupado)
            sql_update = "UPDATE cubiculos SET estado = 'Ocupado', timestamp_ultima_actualizacion = %s WHERE nombre = %s AND estado IN ('Pendiente', 'Ocupado')"
            cur.execute(sql_update, (datetime.now(), cubiculo_nombre))
            db.commit()
            logger.info(f"DB: Cubículo {cubiculo_nombre} confirmado como Ocupado.")
            
        elif estado_sensor == 'Libre':
            # IGNORAR EVENTO LIBRE PARA FORZAR COBRO MANUAL
            logger.warning(f"DB: Evento 'Libre' del sensor {cubiculo_nombre} IGNORADO (Cobro Manual Estricto).")
            db.rollback()

    except Exception as e:
        db.rollback()
        logger.error(f"DB: Error al manejar reporte de cubículo {cubiculo_nombre}: {e}")
    finally:
        cur.close()


# ------------------------- LÓGICA DE COBRO DIFERENCIADO -------------------------

def get_tarifas(tipo_vehiculo, cur):
    """Obtiene las tarifas para un tipo de vehículo ('CARRO' o 'MOTO') desde la DB."""
    cur = mysql.connection.cursor()
    sql = "SELECT tarifa_primera_hora, tarifa_hora_subsiguiente FROM tarifas WHERE tipo = %s"
    cur.execute(sql, (tipo_vehiculo,))
    result = cur.fetchone() 
    cur.close()
    return result if result else (0, 0) 

def calcular_cobro_avanzado(minutos_totales, tarifas):
    """Calcula el monto según tarifas variables e incluye TIEMPO DE GRACIA."""
    TARIFA_PRIMERA_HORA, TARIFA_HORA_SUBSECUENTE = tarifas
    minutos_totales = int(minutos_totales)
    monto_total = 0

    if minutos_totales <= TIEMPO_GRACIA_MINUTOS:
        return 0

    if minutos_totales <= 60:
        monto_total = TARIFA_PRIMERA_HORA
    else:
        monto_total = TARIFA_PRIMERA_HORA
        minutos_restantes = minutos_totales - 60 
        horas_subsiguientes = ceil(minutos_restantes / 60.0)
        monto_total += (horas_subsiguientes * TARIFA_HORA_SUBSECUENTE)
        
    return int(monto_total)

def calcular_cobro_activo(hora_ingreso, cubiculo_id, cur):
    """Calcula el cobro activo. Usa la hora de ingreso real para el cálculo."""
    ahora = datetime.now()
    tiempo_estacionado = ahora - hora_ingreso
    minutos = int(tiempo_estacionado.total_seconds() / 60)
    
    temp_cur = mysql.connection.cursor()
    tipo_vehiculo = TIPO_CARRO
    costo = 0.0

    try:
        if cubiculo_id:
            sql_select_tipo = "SELECT tipo_vehiculo FROM cubiculos WHERE id = %s"
            temp_cur.execute(sql_select_tipo, (cubiculo_id,))
            resultado = temp_cur.fetchone()
            if resultado and resultado[0]:
                tipo_vehiculo = resultado[0]
        else:
            sql_select_tipo = """
                SELECT c.tipo_vehiculo FROM cubiculos c
                JOIN registro_cobro rc ON c.registro_cobro_id = rc.id
                WHERE rc.hora_ingreso = %s AND rc.hora_salida IS NULL
            """
            temp_cur.execute(sql_select_tipo, (hora_ingreso,))
            resultado = temp_cur.fetchone()
            if resultado and resultado[0]:
                tipo_vehiculo = resultado[0]
                
        tarifas = get_tarifas(tipo_vehiculo, None) 
        costo = calcular_cobro_avanzado(minutos, tarifas)
    
    except Exception as e:
        logger.error(f"Error al determinar tipo de vehículo para cobro: {e}")
        costo = 0.0
        
    finally:
        temp_cur.close()
    
    return minutos, costo

# ------------------------- TAREAS PROGRAMADAS -------------------------

@scheduler.task('interval', id='limpieza_pendientes_job', seconds=INTERVALO_LIMPIEZA_SEGUNDOS, misfire_grace_time=900)
def limpiar_pendientes_caducados():
    """Cancela las asignaciones ('Pendiente') que han excedido el TIEMPO_GRACIA_MINUTOS."""
    with app.app_context():
        db = mysql.connection
        cur = db.cursor()
        now = datetime.now()
        
        tiempo_limite = now - timedelta(minutes=TIEMPO_GRACIA_MINUTOS)
        
        sql_select = """
            SELECT 
                c.id AS cubiculo_id, 
                c.nombre AS cubiculo_nombre, 
                c.registro_cobro_id AS registro_id, 
                rc.hora_ingreso
            FROM cubiculos c
            JOIN registro_cobro rc ON c.registro_cobro_id = rc.id
            WHERE c.estado = 'Pendiente' AND rc.hora_ingreso < %s
        """
        
        try:
            cur.execute(sql_select, (tiempo_limite,))
            registros_a_cancelar = cur.fetchall()
            
            if not registros_a_cancelar:
                logger.info("Scheduler: No hay asignaciones pendientes caducadas para limpiar.")
                return

            conteo_cancelados = 0
            
            for cubiculo_id, cubiculo_nombre, registro_id, hora_ingreso in registros_a_cancelar:
                
                sql_delete_registro = "DELETE FROM registro_cobro WHERE id = %s"
                cur.execute(sql_delete_registro, (registro_id,))

                sql_update_cubiculo = "UPDATE cubiculos SET estado = 'Libre', timestamp_ultima_actualizacion = %s, registro_cobro_id = NULL, placa = NULL, tipo_vehiculo = NULL WHERE id = %s"
                cur.execute(sql_update_cubiculo, (now, cubiculo_id))
                
                conteo_cancelados += 1
                logger.warning(f"Scheduler: Cancelación AUTOMÁTICA de asignación en {cubiculo_nombre} (Reg ID: {registro_id}). Tiempo excedido.")

            db.commit()
            logger.info(f"Scheduler: Proceso de limpieza finalizado. {conteo_cancelados} asignaciones canceladas.")

        except Exception as e:
            db.rollback()
            logger.error(f"Scheduler: Error durante el proceso de limpieza automática: {e}")
        finally:
            cur.close()
            
@scheduler.task('interval', id='actualizar_display_job', seconds=5, misfire_grace_time=60)
def actualizar_estado_display():
    """Calcula el estado general (libres y ocupación por cubículo) y lo envía al display OLED."""
    with app.app_context():
        cur = connect_db_dict()
        try:
            sql_query = "SELECT nombre, estado, tipo_vehiculo FROM cubiculos WHERE nombre LIKE 'A%' OR nombre LIKE 'B%' ORDER BY nombre ASC"
            cur.execute(sql_query)
            cubiculos_data = cur.fetchall()
            
            libres_carro = 0
            display_data = []
            
            for c in cubiculos_data:
                if c['nombre'].startswith('A') and c['estado'] == 'Libre':
                    libres_carro += 1
                
                display_data.append({
                    'cub': c['nombre'],
                    'est': c['estado']
                })

            payload = json.dumps({
                "libres": libres_carro, 
                "data": display_data
            })

            client_mqtt.publish(TOPIC_DISPLAY_ESTADO_GENERAL, payload, qos=1)
            logger.debug(f"Display: Estado publicado con {libres_carro} cubículos libres (Carros).")
            
        except Exception as e:
            logger.error(f"Error al actualizar estado del display: {e}")
        finally:
            cur.close()


# ------------------------- RUTAS WEB Y API -------------------------

def connect_db_dict():
    try:
        return mysql.connection.cursor(cursorclass=cursors.DictCursor)
    except Exception as e:
        logger.error(f"Error al conectar a la DB: {e}")
        raise e

@app.route('/')
def index():
    return render_template('index.html')

@app.route('/historial')
def historial():
    return render_template('historial.html')

@app.route('/api/estado_parqueadero')
def get_estado():
    cur = connect_db_dict()
    
    search_term = request.args.get('search', '').strip().upper() 
    params = []

    sql_query = """
    SELECT 
        c.id, c.nombre, c.estado, c.tipo_vehiculo, c.placa AS cubi_placa,
        rc.hora_ingreso, rc.id AS registro_id, rc.placa AS reg_placa,
        (SELECT TIMESTAMPDIFF(MINUTE, rc.hora_ingreso, NOW())) as tiempo_minutos_calc
    FROM cubiculos c 
    LEFT JOIN registro_cobro rc ON c.registro_cobro_id = rc.id
    """
    
    if search_term:
        sql_query += " WHERE c.placa LIKE %s OR c.nombre LIKE %s "
        search_pattern = f"%{search_term}%"
        params.append(search_pattern)
        params.append(search_pattern)

    sql_query += " ORDER BY c.nombre ASC"
    
    try:
        cur.execute(sql_query, tuple(params))
        cubiculos_data = cur.fetchall()
    except Exception as e:
        logger.error(f"Error al ejecutar consulta de estado: {e}")
        cur.close()
        return jsonify({'error': 'Error de base de datos al obtener estado'}), 500
    
    estado_parqueadero = []
    
    for cubiculo_data in cubiculos_data:
        # El campo 'placa' ahora contiene el código único (ej: A-001)
        placa_final = cubiculo_data['reg_placa'] if cubiculo_data['reg_placa'] else cubiculo_data['cubi_placa']
        
        minutos = 0
        cobro_actual = 0.0
        hora_ingreso = cubiculo_data['hora_ingreso']
        cubiculo_id = cubiculo_data['id']
        
        if cubiculo_data['registro_id']:
            minutos = cubiculo_data['tiempo_minutos_calc'] if cubiculo_data['tiempo_minutos_calc'] is not None else 0
            
            if cubiculo_data['estado'] == 'Ocupado' or cubiculo_data['estado'] == 'Pendiente':
                
                if hora_ingreso:
                    minutos_calc, cobro_actual = calcular_cobro_activo(hora_ingreso, cubiculo_id, cur=None) 
                    minutos = minutos_calc 
                
        estado_parqueadero.append({
            'nombre': cubiculo_data['nombre'],
            'estado': cubiculo_data['estado'],
            'hora_ingreso': hora_ingreso.strftime('%Y-%m-%d %H:%M:%S') if hora_ingreso else None,
            'registro_id': cubiculo_data['registro_id'],
            'placa': placa_final, 
            'tipo_vehiculo': cubiculo_data['tipo_vehiculo'],
            'tiempo_minutos': minutos,
            'cobro_actual': round(cobro_actual, 2)
        })
    
    cur.close()
    return jsonify(estado_parqueadero)


@app.route('/api/finalizar_cobro', methods=['POST'])
def finalizar_cobro():
    data = request.json
    registro_id = data.get('registro_id')
    
    if not registro_id:
        return jsonify({'success': False, 'message': 'ID de registro no proporcionado'}), 400

    db = mysql.connection
    cur = db.cursor()
    try:
        ahora = datetime.now()

        # 1. Buscar el registro activo en la tabla registro_cobro
        sql_select = "SELECT hora_ingreso, cubiculo_id, placa FROM registro_cobro WHERE id = %s AND hora_salida IS NULL"
        cur.execute(sql_select, (registro_id,))
        resultado = cur.fetchone()
        
        if not resultado:
            logger.warning(f"Intento de finalizar cobro para registro no activo: {registro_id}")
            return jsonify({'success': False, 'message': 'Registro activo no encontrado'}), 404
            
        hora_ingreso, cubiculo_id, placa = resultado
        
        minutos, monto = calcular_cobro_activo(hora_ingreso, cubiculo_id, cur) 
        
        sql_update_cobro = "UPDATE registro_cobro SET hora_salida = %s, tiempo_total_minutos = %s, monto_cobrado = %s WHERE id = %s"
        cur.execute(sql_update_cobro, (ahora, minutos, monto, registro_id))

        sql_update_cubiculo = "UPDATE cubiculos SET estado = 'Libre', timestamp_ultima_actualizacion = %s, registro_cobro_id = NULL, placa = NULL, tipo_vehiculo = NULL WHERE id = %s"
        cur.execute(sql_update_cubiculo, (ahora, cubiculo_id))

        db.commit()
        cur.close()
        
        logger.info(f"Cobro manual finalizado para Código {placa} (Reg ID: {registro_id}). Monto: {monto}")

        return jsonify({
            'success': True, 
            'message': 'Cobro finalizado y cubículo liberado manualmente.',
            'monto': round(monto, 2),
            'minutos': minutos
        })

    except Exception as e:
        db.rollback()
        cur.close()
        logger.error(f"Error al finalizar cobro manualmente (Reg ID: {registro_id}): {e}")
        return jsonify({'success': False, 'message': str(e)}), 500

@app.route('/api/editar_placa', methods=['POST'])
def editar_placa():
    data = request.json
    registro_id = data.get('registro_id')
    nueva_placa = data.get('nueva_placa')
    
    if not all([registro_id, nueva_placa]):
        return jsonify({'success': False, 'message': 'Datos incompletos'}), 400

    db = mysql.connection
    cur = db.cursor()
    try:
        placa_sanitizada = nueva_placa.upper().strip()
        
        sql_cobro = "UPDATE registro_cobro SET placa = %s WHERE id = %s"
        cur.execute(sql_cobro, (placa_sanitizada, registro_id))
        
        sql_cubiculo = "UPDATE cubiculos SET placa = %s WHERE registro_cobro_id = %s"
        cur.execute(sql_cubiculo, (placa_sanitizada, registro_id))

        db.commit()
        cur.close()
        
        logger.info(f"Placa actualizada manualmente para Reg ID {registro_id} a {placa_sanitizada}.")
        return jsonify({'success': True, 'message': f'Código actualizado a {placa_sanitizada}.'})

    except Exception as e:
        db.rollback()
        logger.error(f"Error al editar placa (Reg ID: {registro_id}): {e}")
        return jsonify({'success': False, 'message': str(e)}), 500

@app.route('/api/cancelar-reserva', methods=['POST'])
def cancelar_reserva():
    db = mysql.connection
    cur = db.cursor()
    try:
        data = request.get_json()
        cubiculo_nombre = data.get('cubiculo_nombre')

        if not cubiculo_nombre:
            return jsonify({'success': False, 'message': 'Nombre de cubículo faltante.'}), 400

        sql_select = "SELECT id, registro_cobro_id, placa FROM cubiculos WHERE nombre = %s AND estado IN ('Pendiente', 'Ocupado')"
        cur.execute(sql_select, (cubiculo_nombre,))
        resultado = cur.fetchone()

        if not resultado or not resultado[1]:
            return jsonify({'success': False, 'message': f'El cubículo {cubiculo_nombre} no tiene una asignación activa.'}), 404
            
        cubiculo_id = resultado[0]
        registro_id = resultado[1]
        placa = resultado[2] 


        sql_delete_registro = "DELETE FROM registro_cobro WHERE id = %s"
        cur.execute(sql_delete_registro, (registro_id,))

        sql_update_cubiculo = "UPDATE cubiculos SET estado = 'Libre', timestamp_ultima_actualizacion = %s, registro_cobro_id = NULL, placa = NULL, tipo_vehiculo = NULL WHERE id = %s"
        cur.execute(sql_update_cubiculo, (datetime.now(), cubiculo_id))

        db.commit()
        cur.close()
        
        logger.warning(f"Asignación cancelada manualmente para Cubículo {cubiculo_nombre}, Código {placa} (Reg ID: {registro_id}).")
        return jsonify({'success': True, 'message': f'Asignación del cubículo {cubiculo_nombre} cancelada y liberado.'})

    except Exception as e:
        db.rollback() 
        logger.error(f"Error en API CANCELAR RESERVA para {cubiculo_nombre}: {e}")
        return jsonify({'success': False, 'message': f'Error interno: {e}'}), 500


@app.route('/api/tarifas', methods=['GET', 'POST'])
def tarifas_api():
    cur = connect_db_dict()
    
    if request.method == 'GET':
        cur.execute("SELECT tipo, tarifa_primera_hora, tarifa_hora_subsiguiente FROM tarifas")
        tarifas = cur.fetchall()
        cur.close()
        return jsonify(tarifas)
        
    elif request.method == 'POST':
        data = request.json
        tipo = data.get('tipo')
        tarifa_ph = data.get('tarifa_primera_hora')
        tarifa_hs = data.get('tarifa_hora_subsiguiente')
        
        if not all([tipo, tarifa_ph, tarifa_hs]):
            cur.close()
            return jsonify({'success': False, 'message': 'Datos de tarifa incompletos'}), 400
            
        try:
            tarifa_ph = int(tarifa_ph)
            tarifa_hs = int(tarifa_hs)
            tipo = str(tipo).upper().strip()
            
            sql = """
            INSERT INTO tarifas (tipo, tarifa_primera_hora, tarifa_hora_subsiguiente)
            VALUES (%s, %s, %s)
            ON DUPLICATE KEY UPDATE 
            tarifa_primera_hora = VALUES(tarifa_primera_hora), 
            tarifa_hora_subsiguiente = VALUES(tarifa_hora_subsiguiente)
            """
            cur.execute(sql, (tipo, tarifa_ph, tarifa_hs))
            mysql.connection.commit()
            cur.close()
            logger.info(f"Tarifas para {tipo} actualizadas a PH:{tarifa_ph}, HS:{tarifa_hs}")
            return jsonify({'success': True, 'message': f'Tarifas para {tipo} actualizadas exitosamente'})
        except ValueError:
            cur.close()
            return jsonify({'success': False, 'message': 'Las tarifas deben ser números enteros.'}), 400
        except Exception as e:
            cur.close()
            logger.error(f"Error al actualizar tarifas para {tipo}: {e}")
            return jsonify({'success': False, 'message': str(e)}), 500

@app.route('/api/tarifas_por_cubiculo/<string:cubiculo_nombre>', methods=['GET'])
def get_tarifas_por_cubiculo(cubiculo_nombre):
    cur = mysql.connection.cursor()
    
    cur.execute("SELECT tipo_vehiculo FROM cubiculos WHERE nombre = %s", (cubiculo_nombre,))
    result = cur.fetchone()
    
    tipo_vehiculo = result[0] if result and result[0] else (TIPO_CARRO if cubiculo_nombre.startswith('A') else TIPO_MOTO)
    
    sql = "SELECT tarifa_primera_hora, tarifa_hora_subsiguiente FROM tarifas WHERE tipo = %s"
    cur.execute(sql, (tipo_vehiculo,))
    result = cur.fetchone()
    cur.close()

    if result:
        return jsonify({
            'tipo': tipo_vehiculo,
            'primera_hora': int(result[0]),  
            'subsiguiente': int(result[1])
        })
    
    return jsonify({'error': 'Tarifa no encontrada'}), 404


@app.route('/api/reporte', methods=['GET'])
def get_reporte():
    fecha_inicio = request.args.get('inicio')
    fecha_fin = request.args.get('fin')
    
    cur = connect_db_dict()

    fecha_inicio_param = fecha_inicio if fecha_inicio else None
    fecha_fin_ajustada = None

    if fecha_fin:
        try:
            fecha_fin_dt = datetime.strptime(fecha_fin, '%Y-%m-%d')
            fecha_fin_ajustada = str(fecha_fin_dt + timedelta(days=1)).split()[0]
        except ValueError:
            logger.warning(f"Formato de fecha de fin inválido: {fecha_fin}")
            fecha_fin_ajustada = None 
    
    query_historial = """
    SELECT 
        r.id, 
        c.nombre AS cubiculo, 
        r.placa, 
        r.hora_ingreso, 
        r.hora_salida, 
        r.tiempo_total_minutos, 
        r.monto_cobrado,
        c.tipo_vehiculo
    FROM registro_cobro r
    JOIN cubiculos c ON r.cubiculo_id = c.id
    WHERE r.hora_salida IS NOT NULL
    """
    
    params = []
    
    if fecha_inicio_param:
        query_historial += " AND r.hora_salida >= %s "
        params.append(fecha_inicio_param)

    if fecha_fin_ajustada:
        query_historial += " AND r.hora_salida < %s "
        params.append(fecha_fin_ajustada)

    query_historial += " ORDER BY r.hora_salida DESC"
    
    try:
        cur.execute(query_historial, tuple(params))
        historial = cur.fetchall()
        
        historial_corregido = []
        sumatoria = 0
        
        conteo_tipos = {
            TIPO_CARRO: {'total': 0.0, 'count': 0}, 
            TIPO_MOTO: {'total': 0.0, 'count': 0}
        }
        
        for item in historial:
            registro = dict(item) 
            
            if registro['hora_ingreso']:
                registro['hora_ingreso'] = registro['hora_ingreso'].strftime('%Y-%m-%d %H:%M:%S')
            
            if registro['hora_salida']:
                registro['hora_salida'] = registro['hora_salida'].strftime('%Y-%m-%d %H:%M:%S')
                
            historial_corregido.append(registro)
            
            monto = float(registro['monto_cobrado']) if registro['monto_cobrado'] is not None else 0.0
            
            tipo = registro['tipo_vehiculo']
            if not tipo and registro.get('cubiculo'):
                 if registro['cubiculo'].startswith('A'):
                     tipo = TIPO_CARRO
                 elif registro['cubiculo'].startswith('B'):
                     tipo = TIPO_MOTO
                     
            sumatoria += monto
            
            if tipo in conteo_tipos:
                conteo_tipos[tipo]['total'] += monto
                conteo_tipos[tipo]['count'] += 1
            
        
        cur.close()
        logger.info(f"Reporte generado. Total: {int(sumatoria)} COP. Carros: {conteo_tipos[TIPO_CARRO]['count']}, Motos: {conteo_tipos[TIPO_MOTO]['count']}")

        return jsonify({
            'historial': historial_corregido,
            'sumatoria_total': int(sumatoria),
            'conteo_tipos': {
                TIPO_CARRO: {'total_cobrado': int(conteo_tipos[TIPO_CARRO]['total']), 'cantidad': conteo_tipos[TIPO_CARRO]['count']},
                TIPO_MOTO: {'total_cobrado': int(conteo_tipos[TIPO_MOTO]['total']), 'cantidad': conteo_tipos[TIPO_MOTO]['count']},
            }
        })

    except Exception as e:
        cur.close()
        logger.error(f"ERROR EN API REPORTE: {e}")
        return jsonify({'success': False, 'message': 'Error interno del servidor al consultar la DB.'}), 500


if __name__ == '__main__':
    # Configurar el cliente MQTT
    client_mqtt.username_pw_set(username=MQTT_USER, password=MQTT_PASSWORD)
    client_mqtt.on_connect = on_connect
    client_mqtt.on_message = on_message
    
    # Intentar conectar al broker MQTT
    try:
        client_mqtt.connect(MQTT_BROKER, MQTT_PORT, 60)
        client_mqtt.loop_start() # Inicia el hilo para escuchar mensajes
        logger.info("MQTT Listener iniciado correctamente.")
    except Exception as e:
        logger.error(f"ERROR: No se pudo conectar al broker MQTT {MQTT_BROKER}:{MQTT_PORT}. Asegúrate de que Mosquitto esté corriendo. Error: {e}")

    # Inicia el scheduler y la aplicación Flask
    scheduler.start()
    app.run(host='0.0.0.0', port=5000, debug=True, use_reloader=False)