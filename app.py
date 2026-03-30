from flask import Flask, render_template, request, jsonify
from datetime import datetime, timedelta
import json
import logging

logging.basicConfig(
    level=logging.DEBUG,
    format='%(asctime)s [%(levelname)s] %(message)s'
)
logger = logging.getLogger('sensor')

app = Flask(__name__, template_folder='app/templates', static_folder='app/static')

# Configuration
CONFIG = {
    'VOLTAGE_CONSTANT': 230.0,  # Voltage in volts (constant, configurable)
    'ADC_MAX': 4095,  # 12-bit ADC
    'ADC_REFERENCE_VOLTAGE': 3.3,  # ESP32 ADC reference voltage in volts
    'SENSOR_VCC': 5.0,  # ZMCT103C module supply voltage
    'CURRENT_SENSOR_RESOLUTION': 100,  # mV per Amp (ZMCT103C module sensitivity)
    'CARBON_EMISSION_FACTOR': 0.82,  # kg CO2 per kWh (adjust based on your region)
    'MAX_READINGS_PER_APPLIANCE': 50,  # Last n readings kept for graph; aggregates use counters
    'CALIBRATION_SAMPLES': 30,  # No-load readings collected to auto-detect ADC center
    'ADC_NOISE_THRESHOLD': 15,  # ADC counts within this range of center are treated as 0A
    'CONTROLLER_TIMEOUT_S': 5,  # Seconds of silence before treating controller as disconnected
}

# Appliance definitions (3 sockets)
APPLIANCES = {
    'socket_1': {
        'name': 'Socket 1',
        'icon': '🔌',
        'adc_pin': 'A0',
        'output_pin': 'D0',
        'color': '#FFD700'
    },
    'socket_2': {
        'name': 'Socket 2',
        'icon': '🔌',
        'adc_pin': 'A1',
        'output_pin': 'D1',
        'color': '#87CEEB'
    },
    'socket_3': {
        'name': 'Socket 3',
        'icon': '🔌',
        'adc_pin': 'A2',
        'output_pin': 'D2',
        'color': '#FF6B6B'
    },
}

# Per-appliance ADC calibration state.
# On startup, the first CALIBRATION_SAMPLES readings (assumed no-load) are averaged
# to find the true zero-current ADC center for each channel.  This accounts for the
# ZMCT103C module's actual DC offset, potentiometer setting, and component tolerances.
adc_calibration = {
    appliance_id: {
        'center': None,
        'samples': [],
    }
    for appliance_id in APPLIANCES
}

# In-memory storage for real-time data and state
# sync_skip_until: after a dashboard toggle, we don't overwrite is_on from controller until this time (so all clients see the same relay state)
appliance_data = {
    appliance_id: {
        'readings': [],
        'total_energy': 0,  # in kWh
        'total_carbon': 0,  # in kg CO2
        'is_on': False,  # Control state (synced from controller so all users see same state)
        'last_timestamp': None,  # Track last reading time
        'is_configured': False,  # Device configured status
        'sync_skip_until': None  # Don't overwrite is_on from controller before this datetime
    }
    for appliance_id in APPLIANCES
}


def _calibration_status(appliance_id):
    """Return calibration progress for a single appliance."""
    cal = adc_calibration[appliance_id]
    if cal['center'] is not None:
        return {'is_calibrating': False, 'progress': 0, 'total': 0, 'center': cal['center']}
    progress = len(cal['samples'])
    total = CONFIG['CALIBRATION_SAMPLES']
    return {'is_calibrating': True, 'progress': progress, 'total': total, 'center': None}


@app.route('/')
def dashboard():
    """Display the main dashboard with all appliances."""
    pin_to_appliance = {appliance_info['output_pin']: aid for aid, appliance_info in APPLIANCES.items()}
    return render_template('dashboard.html', appliances=APPLIANCES, pin_to_appliance=pin_to_appliance)


@app.route('/appliance/<appliance_id>')
def appliance_detail(appliance_id):
    """Display detailed view for a specific appliance."""
    if appliance_id not in APPLIANCES:
        return "Appliance not found", 404
    
    appliance = APPLIANCES[appliance_id]
    appliance['id'] = appliance_id
    
    return render_template('appliance_detail.html', appliance=appliance)


@app.route('/process-data', methods=['POST'])
def process_data():
    """
    Process ADC data and optional digital pin states from ESP32 microcontroller.
    Expected JSON format:
    {
        "A0": 2048,  # ADC reading for pin A0
        "A1": 1024,
        ...
        "D0": 1, "D1": 0, ...  # optional: current relay states (0/1) from controller
    }
    Controller-reported D0-D8 are the source of truth for is_on so all users see the same
    relay state. For a short window after a dashboard toggle we skip overwriting is_on
    from the request (so the toggle is not immediately reverted before the controller
    applies it and reports back).
    """
    try:
        data = request.get_json()
        if not data:
            return jsonify({'error': 'No data provided'}), 400
        
        now = datetime.now()
        # Sync backend state from controller so all dashboard instances see the same relay state
        for appliance_id, appliance_info in APPLIANCES.items():
            output_pin = appliance_info['output_pin']
            if output_pin not in data:
                continue
            skip_until = appliance_data[appliance_id].get('sync_skip_until')
            if skip_until is not None and now < skip_until:
                continue  # Recently toggled from UI; don't overwrite until controller has applied
            val = data[output_pin]
            appliance_data[appliance_id]['is_on'] = bool(val) if isinstance(val, bool) else (int(val) != 0)
        
        result = {}
        
        # Process each appliance's ADC reading
        for appliance_id, appliance_info in APPLIANCES.items():
            adc_pin = appliance_info['adc_pin']
            
            if adc_pin in data:
                adc_value = int(data[adc_pin])
                
                # Update device configuration status on every request
                appliance_data[appliance_id]['is_configured'] = (adc_value != 0)
                
                # If the controller was silent for longer than CONTROLLER_TIMEOUT_S,
                # treat it as a power-cycle and re-calibrate from fresh samples.
                last_ts = appliance_data[appliance_id]['last_timestamp']
                if last_ts is not None:
                    gap = (now - last_ts).total_seconds()
                    if gap > CONFIG['CONTROLLER_TIMEOUT_S']:
                        adc_calibration[appliance_id] = {'center': None, 'samples': []}
                        appliance_data[appliance_id]['readings'] = []
                        appliance_data[appliance_id]['total_energy'] = 0
                        appliance_data[appliance_id]['total_carbon'] = 0
                        appliance_data[appliance_id]['last_timestamp'] = None
                        logger.info(
                            "[TIMEOUT] %s (%s): no data for %.1fs — recalibrating",
                            appliance_id, adc_pin, gap
                        )

                cal = adc_calibration[appliance_id]

                # --- Auto-calibration: collect no-load samples to find true ADC center ---
                if cal['center'] is None:
                    cal['samples'].append(adc_value)
                    needed = CONFIG['CALIBRATION_SAMPLES']
                    if len(cal['samples']) < needed:
                        logger.info(
                            "[CALIBRATING] %s (%s): sample %d/%d  raw_adc=%d",
                            appliance_id, adc_pin, len(cal['samples']), needed, adc_value
                        )
                        # While calibrating, report zero current/power
                        current_a = 0.0
                        power_w = 0.0
                        voltage = CONFIG['VOLTAGE_CONSTANT']
                    else:
                        cal['center'] = int(round(
                            sum(cal['samples']) / len(cal['samples'])
                        ))
                        logger.info(
                            "[CALIBRATED] %s (%s): ADC center = %d  (from %d samples, "
                            "min=%d, max=%d)",
                            appliance_id, adc_pin, cal['center'], needed,
                            min(cal['samples']), max(cal['samples'])
                        )
                        # Fall through to normal processing below

                if cal['center'] is not None:
                    adc_center = cal['center']
                    adc_offset = adc_value - adc_center

                    # Noise dead zone: treat small offsets around center as 0A
                    if abs(adc_offset) <= CONFIG['ADC_NOISE_THRESHOLD']:
                        adc_offset = 0

                    adc_step_mv = (CONFIG['ADC_REFERENCE_VOLTAGE'] * 1000) / CONFIG['ADC_MAX']
                    sensor_output_mv = adc_offset * adc_step_mv

                    current_a = max(0.0, abs(sensor_output_mv) / CONFIG['CURRENT_SENSOR_RESOLUTION'])
                    voltage = CONFIG['VOLTAGE_CONSTANT']
                    power_w = max(0.0, voltage * current_a)

                    logger.debug(
                        "[SENSOR] %s (%s): raw_adc=%d  center=%d  offset=%d  "
                        "sensor_mv=%.1f  current=%.3fA  power=%.1fW",
                        appliance_id, adc_pin, adc_value, adc_center, adc_offset,
                        sensor_output_mv, current_a, power_w
                    )
                
                # Calculate time elapsed since last reading (in seconds)
                current_timestamp = datetime.now()
                time_elapsed_seconds = 1.0  # Default to 1 second for first request
                
                # Only use actual time diff if we have a previous timestamp
                if appliance_data[appliance_id]['last_timestamp'] is not None:
                    time_diff = current_timestamp - appliance_data[appliance_id]['last_timestamp']
                    time_elapsed_seconds = time_diff.total_seconds()
                
                # Calculate energy for this interval (in kWh)
                # Energy = (Power in W / 1000 to get kW) * (Time in hours)
                energy_kwh = (power_w / 1000) * (time_elapsed_seconds / 3600)
                
                # Calculate carbon footprint for this interval
                carbon_kg = energy_kwh * CONFIG['CARBON_EMISSION_FACTOR']
                
                # Accumulate total energy and carbon from every request
                appliance_data[appliance_id]['total_energy'] += energy_kwh
                appliance_data[appliance_id]['total_carbon'] += carbon_kg
                
                # Store the reading
                timestamp = current_timestamp.isoformat()
                reading = {
                    'timestamp': timestamp,
                    'adc_value': adc_value,
                    'voltage': round(voltage, 3),
                    'current': round(current_a, 3),
                    'power': round(power_w, 3),
                    'energy': round(energy_kwh, 6),
                    'carbon': round(carbon_kg, 6)
                }
                
                # Keep only last n readings for graph; aggregates use total_energy / total_carbon
                max_readings = CONFIG['MAX_READINGS_PER_APPLIANCE']
                if len(appliance_data[appliance_id]['readings']) >= max_readings:
                    appliance_data[appliance_id]['readings'].pop(0)
                appliance_data[appliance_id]['readings'].append(reading)
                # Update last timestamp for next request
                appliance_data[appliance_id]['last_timestamp'] = current_timestamp
                
                result[appliance_id] = reading
        
        # Include control states in response using output pins instead of appliance IDs
        control_states = {APPLIANCES[appliance_id]['output_pin']: appliance_data[appliance_id]['is_on'] for appliance_id in APPLIANCES}
        
        return jsonify({
            'status': 'success',
            'control_states': control_states,
            'timestamp': datetime.now().isoformat()
        }), 200
    
    except Exception as e:
        return jsonify({'error': str(e)}), 400


@app.route('/api/appliance/<appliance_id>/data')
def get_appliance_data(appliance_id):
    """Get real-time data for a specific appliance. Returns last n readings for graph; aggregates are counters."""
    if appliance_id not in APPLIANCES:
        return jsonify({'error': 'Appliance not found'}), 404
    
    data = appliance_data[appliance_id]
    # Readings list is already capped at MAX_READINGS_PER_APPLIANCE; return as-is for graph
    readings = data['readings']
    current_reading = readings[-1] if readings else None
    
    return jsonify({
        'appliance_id': appliance_id,
        'current_reading': current_reading,
        'readings': readings,
        'total_energy': round(data['total_energy'], 6),
        'total_carbon': round(data['total_carbon'], 6),
        'calibration': _calibration_status(appliance_id)
    }), 200


@app.route('/api/appliance/<appliance_id>/reset', methods=['POST'])
def reset_appliance_data(appliance_id):
    """Reset data and calibration for a specific appliance."""
    if appliance_id not in APPLIANCES:
        return jsonify({'error': 'Appliance not found'}), 404
    
    appliance_data[appliance_id] = {
        'readings': [],
        'total_energy': 0,
        'total_carbon': 0,
        'is_on': False,
        'last_timestamp': None,
        'is_configured': False,
        'sync_skip_until': None
    }
    adc_calibration[appliance_id] = {'center': None, 'samples': []}
    logger.info("[RESET] %s: data and calibration cleared — will re-calibrate from next %d samples",
                appliance_id, CONFIG['CALIBRATION_SAMPLES'])
    
    return jsonify({'status': 'success', 'message': f'Data reset for {appliance_id}'}), 200


@app.route('/api/calibrate', methods=['POST'])
def calibrate_sensors():
    """
    Reset ADC calibration so the system re-learns the zero-current center from the
    next CALIBRATION_SAMPLES readings.  Call this with NO LOAD connected.
    Optional JSON body: {"appliance_id": "socket_1"} to calibrate a single channel.
    """
    body = request.get_json(silent=True) or {}
    target = body.get('appliance_id')

    if target and target not in APPLIANCES:
        return jsonify({'error': f'Unknown appliance: {target}'}), 404

    targets = [target] if target else list(APPLIANCES.keys())
    for aid in targets:
        adc_calibration[aid] = {'center': None, 'samples': []}
        appliance_data[aid]['readings'] = []
        appliance_data[aid]['total_energy'] = 0
        appliance_data[aid]['total_carbon'] = 0
        appliance_data[aid]['last_timestamp'] = None

    logger.info("[CALIBRATE] Re-calibrating %s — collecting %d no-load samples per channel",
                ', '.join(targets), CONFIG['CALIBRATION_SAMPLES'])

    return jsonify({
        'status': 'success',
        'message': f'Calibration reset for {", ".join(targets)}. '
                   f'Ensure NO LOAD is connected — the next {CONFIG["CALIBRATION_SAMPLES"]} '
                   f'readings will be used to determine the zero-current center.',
        'appliances': targets
    }), 200


@app.route('/api/appliance/<appliance_id>/toggle', methods=['POST'])
def toggle_appliance(appliance_id):
    """Toggle appliance on/off state. Skip syncing from controller for 2s so this toggle is not overwritten."""
    if appliance_id not in APPLIANCES:
        return jsonify({'error': 'Appliance not found'}), 404
    
    appliance_data[appliance_id]['is_on'] = not appliance_data[appliance_id]['is_on']
    appliance_data[appliance_id]['sync_skip_until'] = datetime.now() + timedelta(seconds=2)
    
    return jsonify({
        'status': 'success',
        'appliance_id': appliance_id,
        'is_on': appliance_data[appliance_id]['is_on']
    }), 200


@app.route('/api/appliance/<appliance_id>/state', methods=['GET'])
def get_appliance_state(appliance_id):
    """Get the current state of an appliance."""
    if appliance_id not in APPLIANCES:
        return jsonify({'error': 'Appliance not found'}), 404
    
    return jsonify({
        'appliance_id': appliance_id,
        'is_on': appliance_data[appliance_id]['is_on'],
        'is_configured': appliance_data[appliance_id]['is_configured']
    }), 200


@app.route('/api/appliances/states', methods=['GET'])
def get_all_states():
    """Get the state of all appliances mapped to their output pins."""
    states = {APPLIANCES[appliance_id]['output_pin']: appliance_data[appliance_id]['is_on'] for appliance_id in APPLIANCES}
    configured = {appliance_id: appliance_data[appliance_id]['is_configured'] for appliance_id in APPLIANCES}
    calibration = {appliance_id: _calibration_status(appliance_id) for appliance_id in APPLIANCES}
    return jsonify({'states': states, 'configured': configured, 'calibration': calibration}), 200


@app.route('/api/aggregates', methods=['GET'])
def get_aggregates():
    """Get aggregated data for all appliances."""
    total_energy = 0
    total_carbon = 0
    total_carbon_footprint = 0  # Sum of current carbon values
    
    for appliance_id in APPLIANCES:
        data = appliance_data[appliance_id]
        total_energy += data['total_energy']
        total_carbon += data['total_carbon']
        
        # Get current carbon footprint if available
        if data['readings']:
            total_carbon_footprint += data['readings'][-1]['carbon']
    
    return jsonify({
        'total_energy': round(total_energy, 6),
        'total_carbon': round(total_carbon, 6),
        'total_carbon_footprint': round(total_carbon_footprint, 6)
    }), 200


@app.route('/config')
def get_config():
    """Get configuration values for the frontend."""
    return jsonify(CONFIG), 200


if __name__ == '__main__':
    app.run(debug=True, host='127.0.0.1', port=5000)
