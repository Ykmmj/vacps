#pragma once

/** Count elements of a C array (JSCFunctionListEntry tables, etc.). */
#define VACPS_COUNTOF(a) (sizeof(a) / sizeof((a)[0]))
