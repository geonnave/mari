#include <nrf.h>
#include <stdio.h>
#include <stdbool.h>

#include "sec.h"

//=========================== defines ==========================================

typedef struct {
    bool dummy;
} edhoc_vars_t;

//=========================== variables ========================================

edhoc_vars_t node_vars = { 0 };

//=========================== prototypes =======================================

//=========================== main =============================================

int main(void)
{
    printf("Hello Blink Node\n");
    int8_t res = 0;

    bl_sec_init();

    while (1) {
        res = bl_sec_edhoc_init();
        if (res != 0) {
            printf("Error sec: %d\n", res);
        }

        __SEV();
        __WFE();
        __WFE();
    }
}
