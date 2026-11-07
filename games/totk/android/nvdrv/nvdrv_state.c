/* Definitions of the state declared in nvdrv_state.h. */
#include "nvdrv_state.h"
nvdrv_fd nvdrv_fds[NVDRV_MAX_FD];
/* nvmap backing memory must be guest-visible.  Reserve a dedicated window
 * below the 39-bit Horizon limit; every allocation is committed in the guest
 * memory accounting so QueryMemory agrees with the pointer the SDK receives. */
uint8_t *nvdrv_guest_area;
