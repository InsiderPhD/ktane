#include <xc.h>
#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"
#include "protocol_module.h"
#include "modules.h"
#include "tick.h"
#include "can.h"
#include "mode.h"
#include "buzzer.h"
#include "argb.h"
#include "lcd.h"
#include "status.h"

/* Total number of modules that can be part of the network. */
#define MODULE_COUNT 16
/* Number of errors in each module to track. */
#define ERROR_COUNT  8

/* How frequent should lost modules be checked for, in 10ms units. */
#define LOST_CHECK_PERIOD   10
/* How frequent should module announcements be, in 10ms units. */
#define ANNOUNCE_PERIOD     20
/* How long should it be before we declare a module missing, in 10ms units. */
#define LOST_PERIOD         40

/* The clock tick that this module should check for lost nodes. */
uint32_t next_lost_check;
/* The clock tick that this module should next announce. */
uint32_t next_announce;

/* Structure for an error received from a module. */
typedef struct {
    uint16_t        code;
    uint8_t         count;
} module_error_t;

/* Structure for a module. */
typedef struct {
    struct {
        unsigned    INUSE :1;
        unsigned    LOST  :1;
    }               flags;
    uint8_t         id;
    uint8_t         mode;
    uint16_t        firmware;
    uint32_t        last_seen;
    
    module_error_t  errors[ERROR_COUNT];    
} module_t;

/* Modules in network. */
module_t modules[MODULE_COUNT];

/* Error state representing error tables. */
uint8_t error_state = ERROR_NONE;
/* Previously reported error state. */
uint8_t error_state_prev = ERROR_NONE;

/* Local function prototypes. */
uint8_t module_find(uint8_t id);
uint8_t module_find_or_create(uint8_t id);

/**
 * Initialise the module database, store self and set up next announcement time.
 */
void modules_initialise(void) {        
    /* Init module structure. */
    for (uint8_t i = 0; i < MODULE_COUNT; i++) {
        modules[i].flags.INUSE = 0;
        
        for (uint8_t j = 0; j < ERROR_COUNT; j++) {
            modules[i].errors[j].code = MODULE_ERROR_NONE;
            modules[i].errors[j].code = 0;
        }
    }
    
    /* Store this module in slot 0. */
    modules[0].flags.INUSE = 1;
    modules[0].flags.LOST = 0;
    modules[0].id = can_get_id();
    modules[0].mode = mode_get();
    
    /* Set next module announce time, offsetting with modulus of CAN ID to 
     * attempt to avoid collisions. */
    next_announce = tick_value + ANNOUNCE_PERIOD + (can_get_id() & 0x0f);
    
    /* Set next lost check. */
    next_lost_check = LOST_CHECK_PERIOD;
}

/**
 * Check to see if any modules have errors, or are lost.
 * 
 * @return true if all modules are healthy
 */
bool modules_no_errors(void) {
    return error_state == ERROR_NONE;
}

/**
 * Service modules needs, including sending announcements and checking for other
 * lost modules.
 */
void module_service(void) {   
    /* Announce self. */
    if (tick_value >= next_announce) {
        /* Set next announce time. */
        next_announce = tick_value + ANNOUNCE_PERIOD;
        
        /* Send module announcement. */
        protocol_module_announcement_send();                              
    }
    
    /* Check for lost nodes frequently. */
    if (tick_value >= next_lost_check) {
        next_lost_check = tick_value + LOST_CHECK_PERIOD;
        
        /* Check for lost nodes. */
        for (uint8_t i = 1; i < MODULE_COUNT && modules[i].flags.INUSE; i++) {
            uint32_t expiryTime = modules[i].last_seen + LOST_PERIOD;
            if (modules[i].flags.INUSE && !modules[i].flags.LOST && (expiryTime < tick_value)) {
                modules[i].flags.LOST = 1;
                module_error_raise(MODULE_ERROR_CAN_LOST_BASE | modules[i].id);
            }
        }        
    }
    
    if (error_state_prev != error_state) {
        status_error(error_state);
        error_state_prev = error_state;
    }
}

/**
 * Find a module in the database.
 * 
 * @param id CAN id
 * @return the modules index, or 0xff if not found
 */
uint8_t module_find(uint8_t id) {
    for (uint8_t i = 0; i < MODULE_COUNT; i++) {
        if (modules[i].flags.INUSE) {
            if (modules[i].id == id) {
                return i;
            }
        } else {
            return 0xff;
        }
    }
    
    return 0xff;
}

/**
 * Find or create a module in the database.
 * 
 * @param id CAN id
 * @return the modules index, or 0xff if none could be created 
 */
uint8_t module_find_or_create(uint8_t id) {
    uint8_t idx = module_find(id);
    
    if (idx != 0xff) {
        return idx;
    }

    for (uint8_t i = 0; i < MODULE_COUNT; i++) {
        if (!modules[i].flags.INUSE) {
            modules[i].flags.INUSE = 1;
            modules[i].id = id;
            return i;
        }      
    }
    
    return 0xff;
}

/**
 * Update the module database to mark a module as seen, including the mode that
 * the module has advertised.
 * 
 * @param id CAN id
 * @param mode modules mode
 */
void module_seen(uint8_t id, uint8_t mode, uint16_t firmware) {
    uint8_t idx = module_find_or_create(id);
    
    if (idx == 0xff) {
        return;
    }
    
    modules[idx].mode = mode;
    modules[idx].firmware = firmware;
    modules[idx].last_seen = tick_value;
    modules[idx].flags.LOST = 0;    
}

/**
 * Raise an error for this module, store in our module database and send packet
 * on CAN bus to let other modules be aware of our error (primarily for puzzle
 * modules to report to controller).
 * 
 * @param code error code to raise
 */
void module_error_raise(uint16_t code) {
    protocol_module_error_send(code);
    module_error_record(can_get_id(), code);
}

/**
 * Update the module database with an error that has been received.
 * 
 * @param id CAN id
 * @param code error code received
 */
void module_error_record(uint8_t id, uint16_t code) {
    uint8_t idx = module_find_or_create(id);
    
    if (idx == 0xff) {
        return;
    }
    
    if (idx == 0) {
        error_state = ERROR_LOCAL;
    } else if (error_state != ERROR_LOCAL){
        error_state = ERROR_REMOTE;
    }
        
    for (uint8_t i = 0; i < ERROR_COUNT; i++) {
        if (modules[idx].errors[i].code == code) {
            modules[idx].errors[i].count++;
            return;
        }
        
        if (modules[idx].errors[i].code == MODULE_ERROR_NONE) {
            modules[idx].errors[i].code = code;
            modules[idx].errors[i].count = 1;
            return;
        }
    }
    
    uint8_t ovr = ERROR_COUNT - 1;
    
    if (modules[idx].errors[ovr].code == MODULE_ERROR_TOO_MANY_ERRORS) {
        modules[idx].errors[ovr].count++;
    } else {
        modules[idx].errors[ovr].code = MODULE_ERROR_TOO_MANY_ERRORS;
        modules[idx].errors[ovr].count = 1;
    }
}