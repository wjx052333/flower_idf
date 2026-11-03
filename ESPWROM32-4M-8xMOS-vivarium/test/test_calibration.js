const assert = require('assert');

function vivarium_calc_soil_humidity(raw_val, raw_dry, raw_wet) {
  if (raw_dry <= raw_wet) {
    raw_dry = 3110;
    raw_wet = 958;
  }
  let hum = (raw_dry - raw_val) * 100.0 / (raw_dry - raw_wet);
  if (hum < 0.0) hum = 0.0;
  if (hum > 100.0) hum = 100.0;
  return hum;
}

// 验证基本计算
assert.strictEqual(vivarium_calc_soil_humidity(3110, 3110, 958), 0.0);
assert.strictEqual(vivarium_calc_soil_humidity(958, 3110, 958), 100.0);
assert.strictEqual(vivarium_calc_soil_humidity(2034, 3110, 958), 50.0);
// 验证截断
assert.strictEqual(vivarium_calc_soil_humidity(3500, 3110, 958), 0.0);
assert.strictEqual(vivarium_calc_soil_humidity(500, 3110, 958), 100.0);
// 验证保护回退
assert.strictEqual(vivarium_calc_soil_humidity(3110, 1000, 2000), 0.0);

console.log('PASS: Soil calibration logic tests pass');
