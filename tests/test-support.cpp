#include <src/plugins/PluginAPI.hpp>
#undef PLUGIN_INIT
#define PLUGIN_INIT testSupportInit
#include <hyprtester/plugin/src/main.cpp>
#undef PLUGIN_INIT
#define PLUGIN_INIT pluginInit

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    auto description = testSupportInit(handle);
    g_pInputManager->updateCapabilities();
    return description;
}
