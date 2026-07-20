#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include "cJSON.h"

static const char *parse_whitespace(const char *ptr) {
    while (*ptr && isspace((unsigned char)*ptr)) ptr++;
    return ptr;
}

static const char *parse_string(const char *ptr, char **out_str) {
    if (*ptr != '"') return NULL;
    ptr++;
    const char *start = ptr;
    while (*ptr && *ptr != '"') ptr++;
    if (*ptr != '"') return NULL;
    int len = ptr - start;
    char *str = (char*)malloc(len + 1);
    if (!str) return NULL;
    memcpy(str, start, len);
    str[len] = '\0';
    *out_str = str;
    return ptr + 1;
}

static const char *parse_number(const char *ptr, double *out) {
    char *end;
    double d = strtod(ptr, &end);
    if (end == ptr) return NULL;
    *out = d;
    return end;
}

static const char *parse_value(const char *ptr, cJSON **out);

static const char *parse_object(const char *ptr, cJSON **out) {
    if (*ptr != '{') return NULL;
    ptr++;
    ptr = parse_whitespace(ptr);
    cJSON *obj = (cJSON*)calloc(1, sizeof(cJSON));
    if (!obj) return NULL;
    obj->type = cJSON_Object;
    cJSON *last = NULL;
    char *key = NULL;   // 移到外部作用域，以便 fail 标签处可以 free
    while (*ptr && *ptr != '}') {
        ptr = parse_whitespace(ptr);
        key = NULL;
        ptr = parse_string(ptr, &key);
        if (!key) goto fail;
        ptr = parse_whitespace(ptr);
        if (*ptr != ':') goto fail;
        ptr++;
        ptr = parse_whitespace(ptr);
        cJSON *value = NULL;
        ptr = parse_value(ptr, &value);
        if (!value) goto fail;
        value->string = key;
        key = NULL;  // 所有权已转移，避免 double free
        if (!last)
            obj->child = value;
        else
            last->next = value;
        value->prev = last;
        last = value;
        ptr = parse_whitespace(ptr);
        if (*ptr == ',') ptr++;
    }
    if (*ptr != '}') goto fail;
    ptr++;
    *out = obj;
    return ptr;
fail:
    if (obj) cJSON_Delete(obj);
    if (key) free(key);
    return NULL;
}

static const char *parse_array(const char *ptr, cJSON **out) {
    if (*ptr != '[') return NULL;
    ptr++;
    ptr = parse_whitespace(ptr);
    cJSON *arr = (cJSON*)calloc(1, sizeof(cJSON));
    if (!arr) return NULL;
    arr->type = cJSON_Array;
    cJSON *last = NULL;
    while (*ptr && *ptr != ']') {
        cJSON *value = NULL;
        ptr = parse_value(ptr, &value);
        if (!value) goto fail;
        if (!last)
            arr->child = value;
        else
            last->next = value;
        value->prev = last;
        last = value;
        ptr = parse_whitespace(ptr);
        if (*ptr == ',') ptr++;
    }
    if (*ptr != ']') goto fail;
    ptr++;
    *out = arr;
    return ptr;
fail:
    cJSON_Delete(arr);
    return NULL;
}

static const char *parse_value(const char *ptr, cJSON **out) {
    ptr = parse_whitespace(ptr);
    if (*ptr == '{') {
        return parse_object(ptr, out);
    } else if (*ptr == '[') {
        return parse_array(ptr, out);
    } else if (*ptr == '"') {
        char *str = NULL;
        ptr = parse_string(ptr, &str);
        if (!str) return NULL;
        cJSON *item = (cJSON*)calloc(1, sizeof(cJSON));
        if (!item) {
            free(str);
            return NULL;
        }
        item->type = cJSON_String;
        item->valuestring = str;
        *out = item;
        return ptr;
    } else if (isdigit(*ptr) || *ptr == '-' || *ptr == '+') {
        double d;
        ptr = parse_number(ptr, &d);
        if (!ptr) return NULL;
        cJSON *item = (cJSON*)calloc(1, sizeof(cJSON));
        if (!item) return NULL;
        item->type = cJSON_Number;
        item->valuedouble = d;
        item->valueint = (int)d;
        *out = item;
        return ptr;
    } else if (strncmp(ptr, "true", 4) == 0) {
        cJSON *item = (cJSON*)calloc(1, sizeof(cJSON));
        if (!item) return NULL;
        item->type = cJSON_True;
        *out = item;
        return ptr + 4;
    } else if (strncmp(ptr, "false", 5) == 0) {
        cJSON *item = (cJSON*)calloc(1, sizeof(cJSON));
        if (!item) return NULL;
        item->type = cJSON_False;
        *out = item;
        return ptr + 5;
    } else if (strncmp(ptr, "null", 4) == 0) {
        cJSON *item = (cJSON*)calloc(1, sizeof(cJSON));
        if (!item) return NULL;
        item->type = cJSON_NULL;
        *out = item;
        return ptr + 4;
    }
    return NULL;
}

cJSON *cJSON_Parse(const char *value) {
    if (!value) return NULL;
    cJSON *root = NULL;
    const char *end = parse_value(value, &root);
    if (!end || *end) {
        if (root) cJSON_Delete(root);
        return NULL;
    }
    return root;
}

void cJSON_Delete(cJSON *c) {
    if (!c) return;
    cJSON *next;
    while (c) {
        next = c->next;
        if (c->child) cJSON_Delete(c->child);
        if (c->valuestring) free(c->valuestring);
        if (c->string) free(c->string);
        free(c);
        c = next;
    }
}

cJSON *cJSON_GetObjectItem(const cJSON *object, const char *string) {
    if (!object || !string) return NULL;
    cJSON *child = object->child;
    while (child) {
        if (child->string && strcmp(child->string, string) == 0)
            return child;
        child = child->next;
    }
    return NULL;
}

int cJSON_IsString(const cJSON * const item) {
    return (item && item->type == cJSON_String);
}

int cJSON_IsNumber(const cJSON * const item) {
    return (item && item->type == cJSON_Number);
}
// 判断是否为数组
int cJSON_IsArray(const cJSON * const item) {
    return (item && item->type == cJSON_Array);
}

// 获取数组长度
int cJSON_GetArraySize(const cJSON *array) {
    if (!array || array->type != cJSON_Array) return 0;
    int size = 0;
    cJSON *child = array->child;
    while (child) {
        size++;
        child = child->next;
    }
    return size;
}

// 获取数组中的指定索引元素
cJSON *cJSON_GetArrayItem(const cJSON *array, int index) {
    if (!array || array->type != cJSON_Array) return NULL;
    cJSON *child = array->child;
    int i = 0;
    while (child && i < index) {
        child = child->next;
        i++;
    }
    return child;
}
int cJSON_IsObject(const cJSON * const item) {
    return (item && item->type == cJSON_Object);
}
