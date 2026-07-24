#ifndef MIGRATION_H
#define MIGRATION_H

#include "cJSON.h"

typedef void (*Migration_Complete_Callback)(void);

int Migration_Init(Migration_Complete_Callback callback);
int Migration_Is_Pending(void);
cJSON* Migration_State_JSON(void);

#endif
