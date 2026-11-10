#pragma once

// Optional Robot Hub music capability, gated by CONFIG_BREAD_COMPACT_WIFI_HUB_MUSIC_TEST.
void RegisterHubMusicTools();
void StopHubMusic();
// Called on entering idle; sends at most one automatic host/MCP music request per boot.
void StartHubMusicBootTest();
