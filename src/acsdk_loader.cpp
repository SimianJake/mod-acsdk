/*
 * mod-acsdk script loader. AzerothCore calls Add<module_dir_name>Scripts() with '-' replaced by '_'.
 */

void AddSC_acsdk_world();
void AddSC_acsdk_packets();
void AddSC_acsdk_events();

void Addmod_acsdkScripts()
{
    AddSC_acsdk_world();
    AddSC_acsdk_packets();
    AddSC_acsdk_events();
}
