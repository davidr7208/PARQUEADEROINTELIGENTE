// static/js/monitor.js

let selectedCubiculo = null; // Variable global para el cubículo seleccionado

// 🆕 NUEVAS FUNCIONES PARA BÚSQUEDA
function handleSearchInput(event) {
    // Permite buscar al presionar Enter
    if (event.key === 'Enter') {
        searchCubicles();
    }
}

function searchCubicles() {
    const searchTerm = document.getElementById('search-input').value.trim();
    // Llama a la función principal con el término de búsqueda
    fetchEstadoParqueadero(searchTerm); 
}

function clearSearch() {
    document.getElementById('search-input').value = '';
    // Llama a la función principal sin término de búsqueda
    fetchEstadoParqueadero(); 
}
// ----------------------------------------------------


document.addEventListener('DOMContentLoaded', () => {
    fetchEstadoParqueadero();
    
    // Actualiza el estado cada 5 segundos
    setInterval(() => {
        // 🆕 Mantener el filtro de búsqueda activo en las actualizaciones automáticas
        const searchTerm = document.getElementById('search-input').value.trim();
        fetchEstadoParqueadero(searchTerm); 
    }, 5000); 

    // LÓGICA PARA CERRAR MODAL DE TARIFAS AL HACER CLIC FUERA
    const modalTarifas = document.getElementById('tarifas-modal-backdrop');
    const modalPlaca = document.getElementById('edicion-placa-modal-backdrop');

    if(modalTarifas) {
        modalTarifas.addEventListener('click', (event) => {
            if (event.target === modalTarifas) {
                toggleTarifas(false);
            }
        });
    }
    if(modalPlaca) {
        modalPlaca.addEventListener('click', (event) => {
            if (event.target === modalPlaca) {
                toggleModalEdicion(false);
            }
        });
    }
});

// FUNCIÓN PARA MOSTRAR/OCULTAR EL MODAL DE TARIFAS
function toggleTarifas(mostrar) {
    const modal = document.getElementById('tarifas-modal-backdrop');
    if (modal) {
        if (mostrar) {
            modal.style.display = 'flex';
            fetchTarifas(); 
        } else {
            modal.style.display = 'none';
        }
    }
}


// Función auxiliar para formato de moneda (usado en la simulación de cobro)
function formatCurrency(amount) {
    const numericAmount = parseFloat(amount);
    if (isNaN(numericAmount)) return amount; 
    
    return new Intl.NumberFormat('es-CO', { 
        style: 'currency', 
        currency: 'COP', 
        minimumFractionDigits: 0 
    }).format(numericAmount);
}

// ------------------------- GESTIÓN DE TARIFAS -------------------------

function fetchTarifas() {
    fetch('/api/tarifas')
        .then(response => response.json())
        .then(data => {
            updateTarifasDisplay(data);
        })
        .catch(error => console.error('Error al obtener tarifas:', error));
}

function updateTarifasDisplay(tarifas) {
    const display = document.getElementById('tarifas-display');
    if (!display) return;
    display.innerHTML = '';
    
    tarifas.forEach(tarifa => {
        const div = document.createElement('div');
        div.className = 'tarifa-block';
        div.style.marginBottom = '15px';
        
        div.innerHTML = `
            <h3>Tarifa ${tarifa.tipo}</h3>
            <label for="ph-${tarifa.tipo}">Primera Hora:</label>
            <input type="number" id="ph-${tarifa.tipo}" value="${tarifa.tarifa_primera_hora}">
            
            <label for="hs-${tarifa.tipo}">Hora Subsiguiente:</label>
            <input type="number" id="hs-${tarifa.tipo}" value="${tarifa.tarifa_hora_subsiguiente}">
            
            <button onclick="guardarTarifa('${tarifa.tipo}')">Guardar</button>
        `;
        display.appendChild(div);
    });
}

function guardarTarifa(tipo) {
    const tarifa_ph = document.getElementById(`ph-${tipo}`).value;
    const tarifa_hs = document.getElementById(`hs-${tipo}`).value;

    fetch('/api/tarifas', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ 
            tipo: tipo, 
            tarifa_primera_hora: parseFloat(tarifa_ph), 
            tarifa_hora_subsiguiente: parseFloat(tarifa_hs) 
        })
    })
    .then(response => response.json())
    .then(data => {
        alert(data.message);
        if (data.success) {
            fetchTarifas(); 
        }
    })
    .catch(error => console.error('Error al guardar tarifa:', error));
}


// ------------------------- MONITOREO DE CUBÍCULOS -------------------------

// MODIFICACIÓN: ACEPTAR PARÁMETRO DE BÚSQUEDA
function fetchEstadoParqueadero(searchTerm = '') { 
    // 1. Construir la URL con el parámetro de búsqueda si existe
    let url = '/api/estado_parqueadero';
    if (searchTerm) {
        url += `?search=${encodeURIComponent(searchTerm)}`;
    }

    fetch(url)
        .then(response => response.json())
        .then(data => {
            updateGrid(data);
            updateCobroDetalle(data); 
        })
        .catch(error => console.error('Error al obtener estado del parqueadero:', error));
}

/**
 * Función que renderiza la grilla de cubículos separada por tipo (Carro/Moto).
 */
function updateGrid(cubiculos) {
    const grid = document.getElementById('parqueadero-grid');
    if (!grid) return;
    grid.innerHTML = '';
    
    // 1. Separar cubículos por tipo
    const cubiculosCarro = cubiculos.filter(c => c.nombre.startsWith('A'));
    const cubiculosMoto = cubiculos.filter(c => c.nombre.startsWith('B'));

    const previouslySelected = selectedCubiculo ? selectedCubiculo.nombre : null;
    
    // 2. Función para renderizar un grupo de cubículos
    const renderCubiculoGroup = (group, title) => {
        const section = document.createElement('div');
        section.className = 'cubiculo-group-section';
        // Usamos cubiculo-row que tendrá display: flex y flex-wrap: wrap en CSS
        section.innerHTML = `<h2>${title}</h2><div class="cubiculo-row"></div>`; 
        
        const row = section.querySelector('.cubiculo-row');

        group.forEach(cubiculo => {
            const div = document.createElement('div');
            // Usamos el estado de la DB para la clase CSS (pendiente, ocupado, libre)
            div.className = `cubiculo ${cubiculo.estado.toLowerCase()}`;
            div.dataset.nombre = cubiculo.nombre;
            div.dataset.estado = cubiculo.estado;
            div.dataset.tipo = cubiculo.tipo_vehiculo;
            
            // Mantener la selección visual
            if (cubiculo.nombre === previouslySelected) {
                div.classList.add('selected');
                selectedCubiculo = cubiculo; // Actualizar la data del cubículo seleccionado
            }
            
            div.onclick = () => selectCubiculo(cubiculo);

            let svgFileName = '';
            let estadoDisplay = cubiculo.estado;

            // 1. Determinar el ícono base (carro o moto)
            if (cubiculo.nombre.startsWith('A')) {
                svgFileName = 'carro.svg'; 
            } else if (cubiculo.nombre.startsWith('B')) {
                svgFileName = 'moto.svg'; 
            }
            
            // 2. Ajustar el estado de visualización para la interfaz
            if (cubiculo.estado === 'Pendiente') {
                // El estado 'Pendiente' en DB se muestra como 'Asignado' en la interfaz
                estadoDisplay = 'Asignado'; 
            } else if (cubiculo.estado === 'Ocupado') {
                 estadoDisplay = 'Ocupado';
            } else {
                 estadoDisplay = 'Libre';
            }
            
            const svgPath = `/static/img/icons/${svgFileName}`;

            let placaDisplay = cubiculo.placa ? `<span class="placa">${cubiculo.placa}</span>` : '';
            
            div.innerHTML = `
                <span class="cubiculo-icon">
                    <img src="${svgPath}" alt="${cubiculo.nombre}">
                </span>
                <strong>${cubiculo.nombre}</strong>
                <p>${estadoDisplay}</p>
                ${placaDisplay}
            `;
            row.appendChild(div);
        });
        
        grid.appendChild(section);
    };

    // 3. Renderizar ambas secciones
    renderCubiculoGroup(cubiculosCarro, 'Cubículos para Carros');
    renderCubiculoGroup(cubiculosMoto, 'Cubículos para Motos');
    
    // 4. Ajustar el contenedor principal para que apile las secciones verticalmente
    grid.style.display = 'flex';
    grid.style.flexDirection = 'column';
    grid.style.gap = '20px';
}


// ------------------------- DETALLE DE COBRO ACTIVO -------------------------

/**
 * Función que se ejecuta al hacer clic en un cubículo, 
 * y actualiza automáticamente el panel de detalles de cobro y desplaza la vista.
 *
 */
function selectCubiculo(cubiculo) {
    // 1. Limpiar la selección visual anterior
    document.querySelectorAll('.cubiculo').forEach(div => {
        div.classList.remove('selected');
    });

    selectedCubiculo = cubiculo;
    const detallePanel = document.getElementById('detalle-cobro-activo');
    const panelTitle = document.querySelector('#cobro-info h2'); 
    const cobroInfoContainer = document.getElementById('cobro-info'); // Referencia al contenedor de detalles
    
    // 2. Resaltar el cubículo seleccionado
    const selectedDiv = document.querySelector(`[data-nombre="${cubiculo.nombre}"]`);
    if (selectedDiv) {
        selectedDiv.classList.add('selected');
    }

    panelTitle.textContent = `Detalles de Cubículo: ${cubiculo.nombre}`;
    document.getElementById('cobro-placa').textContent = cubiculo.placa || 'N/A';
    document.getElementById('registro-id-activo').value = cubiculo.registro_id || '';
    
    const infoCobroContainer = document.getElementById('info-cobro-container');
    const botonesAccionContainer = document.getElementById('botones-accion-container');
    const defaultMessage = document.querySelector('#cobro-info p');
    
    defaultMessage.style.display = 'none';
    
    // 3. Lógica por estado: OCUPADO o PENDIENTE
    if (cubiculo.registro_id) { 
        detallePanel.style.display = 'block';
        document.getElementById('cobro-nombre').textContent = cubiculo.nombre;

        // Inicializar inputs del modal de edición
        const modalPlacaInput = document.getElementById('modal-placa');
        if (modalPlacaInput) modalPlacaInput.value = cubiculo.placa || ''; 
        const registroIdEdicionInput = document.getElementById('registro-id-edicion');
        if (registroIdEdicionInput) registroIdEdicionInput.value = cubiculo.registro_id;

        if (cubiculo.estado === 'Ocupado') {
            // Estado Ocupado: Muestra cobro y botones de finalizar/imprimir
            infoCobroContainer.style.display = 'block';
            document.getElementById('cobro-ingreso').textContent = cubiculo.hora_ingreso;
            
            const minutos = cubiculo.tiempo_minutos;
            const horas = (minutos / 60).toFixed(1); 
            document.getElementById('cobro-tiempo').textContent = `${minutos} minutos (${horas} horas)`; 
            document.getElementById('cobro-monto').textContent = formatCurrency(cubiculo.cobro_actual); 
            
            botonesAccionContainer.innerHTML = `
                <button onclick="simularCobro()">Finalizar Cobro</button>
                <button onclick="imprimirVoucher()">Imprimir Ticket Entrada</button>
            `;
            
        } else if (cubiculo.estado === 'Pendiente') { 
            // Estado Pendiente (Asignado): Oculta info de cobro y muestra botón de cancelación
            infoCobroContainer.style.display = 'none';
            
            botonesAccionContainer.innerHTML = `
                <button onclick="cancelarReserva('${cubiculo.nombre}')" class="btn-cancelar">❌ Cancelar Asignación</button>
                <button onclick="imprimirVoucher()">Imprimir Ticket Entrada</button>
            `;
            
        }
    } else {
        // Cubículo Libre sin registro activo
        detallePanel.style.display = 'none';
        defaultMessage.style.display = 'block';
    }

    // 4. Desplazar la vista al panel de detalles
    if (cobroInfoContainer) {
        cobroInfoContainer.scrollIntoView({ 
            behavior: 'smooth', 
            block: 'start' 
        });
    }
}

/** * Función auxiliar para mantener la interfaz de cobro actualizada 
 * si el cubículo activo es el mismo que se está actualizando.
 */
function updateCobroDetalle(cubiculos) {
    // Si hay un cubículo previamente seleccionado
    if (selectedCubiculo) {
        const currentData = cubiculos.find(c => c.nombre === selectedCubiculo.nombre);
        
        // Si el cubículo todavía existe en los datos
        if (currentData) {
             // Actualiza la selección local con los datos más frescos
             selectedCubiculo = currentData;
             
             // Si el panel de detalles está visible
             if (document.getElementById('detalle-cobro-activo').style.display === 'block') {
                 
                // Actualizar la placa (si fue editada)
                document.getElementById('cobro-placa').textContent = currentData.placa || 'N/A';
                
                // Actualizar info si está Ocupado
                if (currentData.estado === 'Ocupado') {
                    document.getElementById('info-cobro-container').style.display = 'block';
                    const minutos = currentData.tiempo_minutos;
                    const horas = (minutos / 60).toFixed(1); 
                    document.getElementById('cobro-tiempo').textContent = `${minutos} minutos (${horas} horas)`; 
                    document.getElementById('cobro-monto').textContent = formatCurrency(currentData.cobro_actual);
                } else if (currentData.estado === 'Pendiente') {
                    // Si pasa a Pendiente o sigue Pendiente, ocultar info de cobro
                    document.getElementById('info-cobro-container').style.display = 'none';
                }
             }
        }
        
        // Si el cubículo fue liberado (no tiene registro_id)
        if (currentData && !currentData.registro_id) {
            hideCobroDetails();
        }
    }
}

function hideCobroDetails() {
     document.getElementById('detalle-cobro-activo').style.display = 'none';
     document.getElementById('cobro-nombre').textContent = '';
     document.getElementById('registro-id-activo').value = ''; 
     document.getElementById('cobro-placa').textContent = ''; 
     document.querySelector('#cobro-info p').style.display = 'block';

     document.querySelectorAll('.cubiculo').forEach(div => {
        div.classList.remove('selected');
    });
    selectedCubiculo = null; // Limpiar la variable global
}

function simularCobro() {
    const registroId = document.getElementById('registro-id-activo').value;
    
    if (!registroId) {
        alert("Seleccione un registro de cobro activo primero.");
        return;
    }

    if (!confirm("¿Está seguro de finalizar el cobro y liberar el cubículo?")) {
        return;
    }

    fetch('/api/finalizar_cobro', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify({ registro_id: registroId })
    })
    .then(response => response.json())
    .then(data => {
        if (data.success) {
            alert(`Cobro finalizado. Monto total: ${formatCurrency(data.monto)} por ${data.minutos} minutos.`);
            fetchEstadoParqueadero(); 
            hideCobroDetails();
        } else {
            alert(`Error al finalizar cobro: ${data.message}`);
        }
    })
    .catch(error => {
        console.error('Error al enviar la solicitud de cobro:', error);
        alert('Error de conexión con el servidor.');
    });
}


// ------------------------- CANCELAR RESERVA -------------------------

function cancelarReserva(cubiculoNombre) {
    if (!confirm(`¿Está seguro que desea cancelar la asignación del cubículo ${cubiculoNombre} y liberarlo? Esta acción eliminará el registro de entrada.`)) {
        return; 
    }

    fetch('/api/cancelar-reserva', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify({ cubiculo_nombre: cubiculoNombre }) 
    })
    .then(response => response.json())
    .then(data => {
        if (data.success) {
            alert(data.message);
            fetchEstadoParqueadero(); 
            hideCobroDetails(); 
        } else {
            alert(`Error al cancelar: ${data.message}`);
        }
    })
    .catch(error => {
        console.error('Error al cancelar reserva:', error);
        alert('Error de conexión con el servidor al intentar cancelar la reserva.');
    });
}

// ------------------------- EDICIÓN MANUAL DE PLACA y IMPRESIÓN -------------------------

function toggleModalEdicion(mostrar) {
    const modal = document.getElementById('edicion-placa-modal-backdrop');
    if (modal) {
        if (mostrar && selectedCubiculo) {
            // Cargar datos actuales en el modal antes de mostrar
            document.getElementById('modal-placa').value = selectedCubiculo.placa || '';
            document.getElementById('registro-id-edicion').value = selectedCubiculo.registro_id || '';
        }
        modal.style.display = mostrar ? 'flex' : 'none';
    }
}

function guardarEdicionPlaca() {
    const registroId = document.getElementById('registro-id-edicion').value;
    const nuevaPlaca = document.getElementById('modal-placa').value.toUpperCase().trim();
    
    if (!registroId || !nuevaPlaca) {
        alert("ID de registro o placa no válida.");
        return;
    }

    fetch('/api/editar_placa', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify({ registro_id: registroId, nueva_placa: nuevaPlaca })
    })
    .then(response => response.json())
    .then(data => {
        alert(data.message);
        if (data.success) {
            toggleModalEdicion(false);
            fetchEstadoParqueadero(); // Refrescar la grilla y el detalle
        }
    })
    .catch(error => {
        console.error('Error al editar placa:', error);
        alert('Error de conexión con el servidor al intentar editar la placa.');
    });
}

async function imprimirVoucher() {
    if (!selectedCubiculo || !selectedCubiculo.registro_id) {
        alert("Seleccione un cubículo ocupado o asignado para imprimir el ticket.");
        return;
    }
    
    // Obtener las tarifas para incluirlas en el voucher
    let tarifas = { primera_hora: 'N/A', subsiguiente: 'N/A' };
    try {
        const response = await fetch(`/api/tarifas_por_cubiculo/${selectedCubiculo.nombre}`);
        if (response.ok) {
            tarifas = await response.json();
        }
    } catch (e) {
        console.error("Error al obtener tarifas para el voucher:", e);
    }
    
    const cubiculoNombre = selectedCubiculo.nombre;
    const horaIngresoCompleta = selectedCubiculo.hora_ingreso || 'N/A';
    const [fechaIngreso, horaIngreso] = horaIngresoCompleta.split(' ');
    const registroId = selectedCubiculo.registro_id || 'N/A';
    const placaActual = selectedCubiculo.placa || 'N/A';
    const tipoVehiculo = selectedCubiculo.tipo_vehiculo || (cubiculoNombre.startsWith('A') ? 'CARRO' : 'MOTO');

    const voucherHTML = `
        <div class="voucher-content" style="width: 250px; padding: 15px; font-family: 'Courier New', monospace; font-size: 12px; margin: 0 auto; line-height: 1.5;">
            <h3 style="text-align: center; margin: 0; padding-bottom: 5px; border-bottom: 1px dashed #000;">
                PARQUEADERO INTELIGENTE
            </h3>
            <p style="text-align: center; margin-top: 5px;">TICKET DE ENTRADA</p>
            
            <p style="border-top: 1px dashed #000; padding-top: 10px;">
                <strong>TICKET N°:</strong> ${registroId}<br>
                <strong>PLACA:</strong> ${placaActual}<br>
                <strong>CUBÍCULO ASIGNADO:</strong> ${cubiculoNombre}<br>
                <strong>TIPO:</strong> ${tipoVehiculo}<br>
            </p>

            <p>
                <strong>HORA INGRESO:</strong> ${fechaIngreso} ${horaIngreso}<br>
            </p>

            <h4 style="border-top: 1px dashed #000; padding-top: 10px; text-align: center;">
                TARIFAS
            </h4>
            <p>
                <strong>1ª Hora:</strong> ${formatCurrency(tarifas.primera_hora)}<br>
                <strong>Subsiguiente:</strong> ${formatCurrency(tarifas.subsiguiente)}/hr<br>
            </p>
            
            <p style="text-align: center; margin-top: 15px; font-size: 10px; border-top: 1px dashed #000; padding-top: 10px;">
                ¡GRACIAS POR SU VISITA!
            </p>
        </div>
    `;

    const printWindow = window.open('', '_blank', 'height=600,width=350');
    printWindow.document.write('<html><head><title>Ticket de Entrada</title>');
    printWindow.document.write(`
        <style>
            @media print {
                body { margin: 0; padding: 0; }
                .voucher-content { 
                    border: none !important; 
                    box-shadow: none !important;
                }
            }
        </style>
    `);
    printWindow.document.write('</head><body>');
    printWindow.document.write(voucherHTML);
    printWindow.document.write('</body></html>');
    printWindow.document.close();

    printWindow.onload = function() {
        printWindow.print();
    }
}