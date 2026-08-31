#include <cassert>
#include <cstdint>
#include <iostream>

#include "../../src/sensor_debounce.h"

int main() {
  SensorDebounce sensor(400);
  sensor.begin(true, 0);
  assert(sensor.raw() && sensor.stable());

  // Parasites de 10 ms et 100 ms : aucun changement stable.
  assert(!sensor.update(false, 100));
  assert(!sensor.update(true, 110));
  assert(sensor.stable());
  assert(!sensor.update(false, 200));
  assert(!sensor.update(true, 300));
  assert(sensor.stable());

  // Plusieurs rebonds, puis une vraie ouverture maintenue plus de 400 ms.
  assert(!sensor.update(false, 400));
  assert(!sensor.update(true, 450));
  assert(!sensor.update(false, 500));
  assert(!sensor.update(false, 899));
  assert(sensor.update(false, 900));
  assert(!sensor.stable());

  // Etat identique répété : aucune nouvelle transition.
  assert(!sensor.update(false, 1500));

  // Vraie fermeture maintenue plus de 400 ms.
  assert(!sensor.update(true, 1600));
  assert(!sensor.update(true, 1999));
  assert(sensor.update(true, 2000));
  assert(sensor.stable());
  assert(sensor.bounceCount() == 3);

  std::cout << "sensor debounce tests: PASS\n";
  return 0;
}
