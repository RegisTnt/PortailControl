#pragma once

// Commandes RF locales actives ; protections conservees via activerRelais().
constexpr bool RF_RELAY_CONTROL_ENABLED = true;

namespace RF433 {
using ButtonHandler = void (*)(char button);
void begin(ButtonHandler handler);
void handle();
}
