#include "panda/plugin.h"
#include "panda/plugin_plugin.h"

#include "dependency_network_def.h"

bool init_plugin(void *self) {
	printf("Hello World from Dependency_Network Plugin.");

	return true;
}

void uninit_plugin(void *self) {
	printf("Goodbye World from Dependency_Network Plugin.");
}