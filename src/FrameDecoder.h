#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

constexpr size_t MAX_BITS = 80;

// Empreinte lisible, sans hash : P = preambule bas, N = aucun preambule.
// 0 = HAUT court / BAS long ; 1 = HAUT long / BAS court.
inline bool validFingerprint(const char* text) {
  const size_t length = strlen(text);
  if (length < 17 || length > MAX_BITS + 1 || (text[0] != 'P' && text[0] != 'N')) return false;
  for (size_t i = 1; i < length; ++i)
    if (text[i] != '0' && text[i] != '1') return false;
  return true;
}

#if __cplusplus >= 201402L
constexpr
#endif
inline bool normalizeFrame(const int32_t* pulses, size_t count, char* out) {
  if (!count) return false;
  size_t first = 0;
  out[0] = 'N';
  if (pulses[0] <= -1500 && pulses[0] >= -4000) {
    first = 1;
    out[0] = 'P';
  }
  const size_t remaining = count - first;
  if (remaining % 2 || remaining < 32 || remaining > MAX_BITS * 2) return false;
  size_t bit = 1;
  for (size_t i = first; i < count; i += 2) {
    const int32_t high = pulses[i], low = -pulses[i + 1];
    if (high < 200 || high > 1100 || low < 200 || low > 1100) return false;
    if (high + low < 950 || high + low > 1550) return false;
    const bool highLong = high > 600, lowLong = low > 600;
    if (highLong == lowLong) return false;
    out[bit++] = highLong ? '1' : '0';
  }
  out[bit] = 0;
  return true;
}
