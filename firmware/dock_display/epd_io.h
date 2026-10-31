// Push the ImageBW buffer to the panel. Device only.
#pragma once

void epdPowerOn();
// Full refresh: flashes black/white to clear ghosting. Use on timer wakes.
void epdShowFull();
// Partial refresh: no flash. Use for button navigation. Falls back to full
// every FULL_REFRESH_EVERY updates.
void epdShowPartial();
