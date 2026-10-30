// Flash-Speicher für die Daten des privaten Bereichs (zweitletzter Sektor des Flash).
// Wie bei den Einstellungen meldet Kern 1 die Änderung, Kern 0 schreibt sie nach kurzer Ruhe.

#pragma once

#include <stdint.h>

#include "private_data.h"

void priv_load(priv_data_t *d);
void priv_request_save(const priv_data_t *d, uint32_t now_ms);  // Kern 1
void priv_service(uint32_t now_ms);                             // Kern 0
