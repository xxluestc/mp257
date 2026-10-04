#ifndef cJSON__h
#define cJSON__h
#ifdef __cplusplus
extern "C" {
#endif
#include <stddef.h>
typedef struct cJSON { struct cJSON *next,*prev,*child; int type; char *valuestring; int valueint; double valuedouble; char *string; } cJSON;
#define cJSON_False 0
#define cJSON_True 1
#define cJSON_NULL 2
#define cJSON_Number 3
#define cJSON_String 4
#define cJSON_Array 5
#define cJSON_Object 6
cJSON *cJSON_Parse(const char *value);
void cJSON_Delete(cJSON *c);
cJSON *cJSON_GetObjectItem(const cJSON *object, const char *string);
int cJSON_IsString(const cJSON * const item);
int cJSON_IsNumber(const cJSON * const item);
extern int cJSON_IsArray(const cJSON * const item);
extern int cJSON_GetArraySize(const cJSON *array);
extern cJSON *cJSON_GetArrayItem(const cJSON *array, int index);
extern int cJSON_IsObject(const cJSON * const item);
#ifdef __cplusplus
}
#endif
#endif
