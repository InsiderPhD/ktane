#ifndef MODE_H
#define	MODE_H

#define MODE_BLANK               0
#define MODE_BOOTSTRAP           1
#define MODE_UNCONFIGURED        2
#define MODE_CONTROLLER          3
#define MODE_CONTROLLER_STANDBY  4
#define MODE_PUZZLE_DEBUG        5

uint8_t mode_get(void);
void mode_initialise(void);
void mode_service(void);
void mode_register_callback(uint8_t stage, void (*func)(bool));

#endif	/* MODE_H */

