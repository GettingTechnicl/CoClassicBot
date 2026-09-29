#pragma once

// =====================================================================
// net_connect_hook.h — Phase 1 Step 2 of the containment project
// (docs/investigation/CONTAINMENT.md): redirects the game's outbound
// connections through the launcher's direct-connect gateway
// (injector/main.cpp's ConnectGateway) instead of letting the game leg go
// direct, bypassing every observability the relay provides.
//
// Gated on the COCLASSIC_GW environment variable, set by launcher.exe only
// for an account launched with proxy mode on. With no such variable (a
// manual injection, or proxy mode off), this stays pure pass-through --
// every hook still attaches, but never rewrites anything.
// =====================================================================

void InitNetConnectHook();
void CleanupNetConnectHook();
