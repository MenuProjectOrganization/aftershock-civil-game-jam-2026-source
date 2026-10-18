#pragma once

class UWorld;

/** Opt-in Development render capture. Never enabled during ordinary gameplay. */
void BeginAftershockPresentationAudit(UWorld& World);
