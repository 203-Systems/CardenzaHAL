#pragma once
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#ifdef __cplusplus
#define ESP_COMPILER_DESIGNATED_INIT_AGGREGATE_TYPE_EMPTY(member)
#else
#define ESP_COMPILER_DESIGNATED_INIT_AGGREGATE_TYPE_EMPTY(member) .member = {},
#endif
#define ESP_STATIC_ASSERT _Static_assert
