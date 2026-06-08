"""
Modbus RTU Module Configurator
Flask backend — serial RTU + TCP connection support
"""

import sys
import os
import time
import logging
import threading
from flask import Flask, render_template, jsonify, request
from flask_cors import CORS

logging.basicConfig(level=logging.INFO)


def resource_path(relative):
    """Works both for development and for PyInstaller .exe"""
    base = getattr(sys, '_MEIPASS', os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base, relative)


app = Flask(__name__,
    template_folder=resource_path('templates'),
    static_folder=resource_path('static'),
)
CORS(app)

# ── Connection state ──────────────────────────────────────────────────────────
_client          = None
_connected_port  = None   # serial port name OR "ip:port" string
_connection_type = None   # 'rtu' or 'tcp'
_lock            = threading.Lock()  # protects all Modbus client access

# HR offsets (1-based Modbus address → 0-based pymodbus address = address - 1)
HR_MODULE_START = 71   # HR[71..89] = module types (19 slots)
HR_TRIGGER      = 91   # Write 1235 to trigger save
HR_CONFIRM      = 92   # Device writes result here
HR_COUNT        = 93   # Device writes moduleCount here after save
SLAVE_ID        = 2
MAX_MODULES     = 19

# Firmware confirmation codes
CONFIRM_SUCCESS  = 5321
CONFIRM_EMPTY    = 1111   # res==1: nothing to save
CONFIRM_ERASE    = 2222   # res==2: flash erase failed
CONFIRM_WRITE    = 3333   # res==3: flash write failed
CONFIRM_INVALID  = 4444   # res==4: invalid module type

CONFIRM_MESSAGES = {
    CONFIRM_EMPTY:   'Empty module list — nothing to save',
    CONFIRM_ERASE:   'Flash erase error',
    CONFIRM_WRITE:   'Flash write error',
    CONFIRM_INVALID: 'Invalid module type in list',
}


def _close_client():
    global _client, _connected_port, _connection_type
    with _lock:
        if _client:
            try:
                _client.close()
            except Exception:
                pass
        _client          = None
        _connected_port  = None
        _connection_type = None


# ── Serial port discovery ──────────────────────────────────────────────────────
@app.route('/api/ports', methods=['GET'])
def list_ports():
    try:
        import serial.tools.list_ports
        ports = [
            {'port': p.device, 'desc': p.description}
            for p in serial.tools.list_ports.comports()
        ]
        return jsonify({'ports': ports})
    except Exception as e:
        return jsonify({'error': str(e)}), 500


# ── Connect ────────────────────────────────────────────────────────────────────
@app.route('/api/connect', methods=['POST'])
def connect():
    global _client, _connected_port, _connection_type
    data = request.get_json()
    mode = data.get('mode', 'rtu')

    _close_client()

    try:
        if mode == 'tcp':
            host = data.get('host', '').strip()
            port = int(data.get('tcp_port', 502))
            if not host:
                return jsonify({'error': 'No IP address specified'}), 400

            from pymodbus.client import ModbusTcpClient
            client = ModbusTcpClient(host=host, port=port, timeout=2)
            if not client.connect():
                return jsonify({'error': f'Could not connect to {host}:{port}'}), 500

            _client          = client
            _connected_port  = f'{host}:{port}'
            _connection_type = 'tcp'
            logging.info(f'TCP connected to {host}:{port}')

        else:
            port = data.get('port', '').strip()
            if not port:
                return jsonify({'error': 'No serial port specified'}), 400

            from pymodbus.client import ModbusSerialClient
            client = ModbusSerialClient(
                port=port,
                baudrate=19200,
                parity='N',
                stopbits=1,
                bytesize=8,
                timeout=1,
            )
            if not client.connect():
                return jsonify({'error': f'Could not open {port}'}), 500

            _client          = client
            _connected_port  = port
            _connection_type = 'rtu'
            logging.info(f'RTU connected to {port}')

        config = _read_config_from_device()
        return jsonify({
            'success':    True,
            'connection': _connected_port,
            'mode':       _connection_type,
            'config':     config,
        })

    except Exception as e:
        _close_client()
        return jsonify({'error': str(e)}), 500


@app.route('/api/disconnect', methods=['POST'])
def disconnect():
    _close_client()
    return jsonify({'success': True})


# ── Read config from device ────────────────────────────────────────────────────
def _read_config_from_device():
    if not _client:
        return [0] * MAX_MODULES
    try:
        with _lock:
            result = _client.read_holding_registers(
                address=HR_MODULE_START - 1,
                count=MAX_MODULES,
                slave=SLAVE_ID
            )
        if result.isError():
            logging.warning('Error reading config registers')
            return [0] * MAX_MODULES
        regs = list(result.registers)
        regs += [0] * (MAX_MODULES - len(regs))
        return regs[:MAX_MODULES]
    except Exception as e:
        logging.error(f'Read config error: {e}')
        return [0] * MAX_MODULES


@app.route('/api/read-config', methods=['GET'])
def read_config():
    if not _client:
        return jsonify({'error': 'Not connected'}), 400
    config = _read_config_from_device()
    return jsonify({'config': config})


# ── Write config to device ─────────────────────────────────────────────────────
@app.route('/api/write-config', methods=['POST'])
def write_config():
    if not _client:
        return jsonify({'error': 'Not connected'}), 400

    data    = request.get_json()
    modules = data.get('modules', [])

    if len(modules) != MAX_MODULES:
        return jsonify({'error': f'Expected {MAX_MODULES} module values'}), 400

    try:
        with _lock:
            # Step 1: clear HR[92] so we don't read a stale confirmation
            _client.write_register(
                address=HR_CONFIRM - 1,
                value=0,
                slave=SLAVE_ID
            )

            # Step 2: write module types to HR[71..89]
            result = _client.write_registers(
                address=HR_MODULE_START - 1,
                values=modules,
                slave=SLAVE_ID
            )
            if result.isError():
                return jsonify({'error': 'Failed to write module registers'}), 500

            # Step 3: trigger save by writing 1235 to HR[91]
            result = _client.write_register(
                address=HR_TRIGGER - 1,
                value=1235,
                slave=SLAVE_ID
            )
            if result.isError():
                return jsonify({'error': 'Failed to write trigger register'}), 500

        # Step 4: release lock while device processes (flash erase+write takes time)
        time.sleep(0.8)

        # Step 5: read confirmation
        with _lock:
            result = _client.read_holding_registers(
                address=HR_CONFIRM - 1,
                count=2,
                slave=SLAVE_ID
            )
            if result.isError():
                return jsonify({'error': 'Failed to read confirmation'}), 500

        confirm = result.registers[0]
        count   = result.registers[1]

        logging.info(f'Write config confirmation: HR[92]={confirm}, HR[93]={count}')

        if confirm == CONFIRM_SUCCESS:
            return jsonify({'success': True, 'moduleCount': count})
        elif confirm in CONFIRM_MESSAGES:
            return jsonify({'error': CONFIRM_MESSAGES[confirm], 'code': confirm}), 500
        elif confirm == 0:
            return jsonify({'error': 'Device did not respond — check connection and try again'}), 500
        else:
            return jsonify({'error': f'Unknown confirmation code: {confirm}'}), 500

    except Exception as e:
        logging.error(f'Write config error: {e}')
        return jsonify({'error': str(e)}), 500


# ── Live data ─────────────────────────────────────────────────────────────────
# Firmware data layout:
#   HR[1]        = ambient temperature: stored as (temp_c + 100), uint16
#   Coils 1..63  = digital inputs, sequential per module
#   HR[2..70]    = analog/counter values, sequential per module
#
#   Per module type:
#     DwCRS (1): 11 coils + 1 HR (CRS counter)
#     Digital(2): 12 coils
#     4-20mA (3): 6 HR values (raw 12-bit ADC, 0=4mA, 4095=20mA)
#     PT100  (4): 4 HR values (uint8 with +30 offset, actual = raw - 30)

@app.route('/api/live-data', methods=['POST'])
def live_data():
    if not _client:
        return jsonify({'error': 'Not connected'}), 400

    data     = request.get_json()
    mod_list = data.get('modules', [])

    try:
        with _lock:
            # Coils: address=0 → coil 1 (firmware starts at 1)
            # coils[0]=coil1, coils[1]=coil2, ..., coils[63]=coil64
            coil_result = _client.read_coils(
                address=0, count=64, slave=SLAVE_ID
            )
            # HR: address=0 → HR[1], address=1 → HR[2], ...
            # hrs[0]=HR1, hrs[1]=HR2, ..., hrs[69]=HR70
            hr_result = _client.read_holding_registers(
                address=0, count=70, slave=SLAVE_ID
            )

        coils = list(coil_result.bits)    if not coil_result.isError() else [False] * 64
        hrs   = list(hr_result.registers) if not hr_result.isError()   else [0] * 70

        # Ambient: HR[1] = temp_c + 100  →  hrs[0]
        raw_temp = hrs[0] if hrs else None
        ambient  = (raw_temp - 100) if raw_temp is not None else None

        # Walk modules sequentially
        # co_idx is 1-based firmware coil number → array index = co_idx - 1
        # hr_idx is 1-based firmware HR number   → array index = hr_idx - 1
        co_idx = 1   # firmware starts coils at 1
        hr_idx = 2   # firmware starts data HRs at 2 (HR[1] is ambient)
        result_modules = []

        for mod_type in mod_list:

            if mod_type == 0:
                result_modules.append({'type': 0})
                continue

            if mod_type == 1:  # DwCRS — 11 coils + 1 HR counter
                bits = []
                for _ in range(11):
                    bits.append(bool(coils[co_idx - 1]) if co_idx <= 64 else False)
                    co_idx += 1
                counter = hrs[hr_idx - 1] if hr_idx <= 70 else 0
                hr_idx += 1
                result_modules.append({'type': 1, 'bits': bits, 'counter': counter})

            elif mod_type == 2:  # Digital — 12 coils
                bits = []
                for _ in range(12):
                    bits.append(bool(coils[co_idx - 1]) if co_idx <= 64 else False)
                    co_idx += 1
                result_modules.append({'type': 2, 'bits': bits})

            elif mod_type == 3:  # 4-20mA — 6 HR values
                values = []
                for _ in range(6):
                    raw = hrs[hr_idx - 1] if hr_idx <= 70 else 0
                    # 12-bit ADC: 0=4mA, 4095=20mA
                    ma = round(4.0 + (raw / 4095.0) * 16.0, 1) if raw > 0 else 0.0
                    values.append({'raw': raw, 'ma': ma})
                    hr_idx += 1
                result_modules.append({'type': 3, 'values': values})

            elif mod_type == 4:  # PT100 — 4 HR values
                values = []
                for _ in range(4):
                    raw = hrs[hr_idx - 1] if hr_idx <= 70 else 0
                    # Stored as uint8 with +30 offset: actual = raw - 30
                    # Range: -20°C (raw=10) to +120°C (raw=150)
                    temp_c = raw - 30
                    values.append({'raw': raw, 'temp': temp_c})
                    hr_idx += 1
                result_modules.append({'type': 4, 'values': values})

        return jsonify({'modules': result_modules, 'ambient': ambient})

    except Exception as e:
        logging.error(f'Live data error: {e}')
        return jsonify({'error': str(e)}), 500


# ── Status ─────────────────────────────────────────────────────────────────────
@app.route('/api/status', methods=['GET'])
def status():
    return jsonify({
        'connected': _client is not None,
        'port':      _connected_port,
        'mode':      _connection_type,
    })


# ── Main page ──────────────────────────────────────────────────────────────────
@app.route('/')
def index():
    return render_template('index.html')


if __name__ == '__main__':
    from cheroot.wsgi import Server as CherootServer
    host    = '127.0.0.1'
    port    = 5001
    threads = 4
    print(f"Starting Modbus Configurator on http://{host}:{port}")
    server = CherootServer(
        (host, port),
        app,
        numthreads=threads,
        max=-1,
        request_queue_size=20
    )
    try:
        server.start()
    except KeyboardInterrupt:
        print("\nShutting down...")
        server.stop()
