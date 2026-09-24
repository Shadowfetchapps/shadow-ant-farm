#include "ant_audio.h"
#include "ant_farm_sim.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

static void initialize_antfarm(ModuleInitializationLevel level)
{
	if (level != MODULE_INITIALIZATION_LEVEL_SCENE)
		return;
	GDREGISTER_CLASS(AntFarmSim);
	GDREGISTER_CLASS(AntAudioSynth);
}

static void uninitialize_antfarm(ModuleInitializationLevel level)
{
	(void)level;
}

extern "C" {
GDExtensionBool GDE_EXPORT antfarm_library_init(GDExtensionInterfaceGetProcAddress get_proc_address, GDExtensionClassLibraryPtr library,
                                                GDExtensionInitialization *initialization)
{
	GDExtensionBinding::InitObject init(get_proc_address, library, initialization);
	init.register_initializer(initialize_antfarm);
	init.register_terminator(uninitialize_antfarm);
	init.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
	return init.init();
}
}
