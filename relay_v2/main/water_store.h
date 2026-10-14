/**
 * water_store.h — Persist the watering configuration in NVS.
 */
#pragma once

#include <stdbool.h>

/* Load the saved configuration into water_config(). Fields that are missing or
 * out of range keep their defaults. Never fails: logs and returns on error. */
void water_store_load(void);

/* Write the whole configuration. Returns false if NVS rejected it; the
 * in-memory configuration stays in effect either way. */
bool water_store_save(void);
