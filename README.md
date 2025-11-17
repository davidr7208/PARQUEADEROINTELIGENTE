# Parqueadero Inteligente

## Descripción
Aplicación que registra ingreso y salida de vehículos (carro, moto), guarda hora de entrada y salida y calcula costo según tiempo en el parqueadero.

## Estructura del repositorio
- `src/` - código fuente
- `docker-compose.yml` - despliegue con Docker Compose
- `Jenkinsfile` - pipeline de CI/CD
- `README.md` - documentación (este archivo)

## Flujo Git
Ramas:
- `main` - versión de producción
- `dev` - integración de funcionalidades
- `feature/*` - ramas por funcionalidad

## Cómo correr localmente (con Docker)
1. Copia archivo `.env` si aplica.
2. Construir y levantar:
```bash
docker-compose up -d --build
