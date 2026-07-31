#pragma once

#include <quickjs.h>

namespace vacps::js {

JSModuleDef* init_module_log(JSContext* ctx, const char* name, void* binding);
JSModuleDef* init_module_store(JSContext* ctx, const char* name, void* binding);
JSModuleDef* init_module_host(JSContext* ctx, const char* name, void* binding);
JSModuleDef* init_module_http(JSContext* ctx, const char* name, void* binding);
JSModuleDef* init_module_fs(JSContext* ctx, const char* name, void* binding);
JSModuleDef* init_module_process(JSContext* ctx, const char* name, void* binding);
JSModuleDef* init_module_crypto(JSContext* ctx, const char* name, void* binding);

}  // namespace vacps::js
