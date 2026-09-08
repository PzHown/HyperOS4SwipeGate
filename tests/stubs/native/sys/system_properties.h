#pragma once
#define PROP_VALUE_MAX 92
#ifdef __cplusplus
extern "C" {
#endif
int __system_property_get(const char *, char *);
#ifdef __cplusplus
}
#endif
