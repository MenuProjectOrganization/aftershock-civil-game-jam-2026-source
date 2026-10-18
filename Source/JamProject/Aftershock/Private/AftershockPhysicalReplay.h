#pragma once
#include "CoreMinimal.h"
struct FAftershockMassVisualEvent;

/** Original bounded SI replay codec, independent of UObject reflection. */
bool EncodeAftershockMassReplay(const FAftershockMassVisualEvent& Event,TArray<uint8>& Out,FText& Error);
bool DecodeAftershockMassReplay(const TArray<uint8>& Bytes,FAftershockMassVisualEvent& Out,FText& Error);
bool ValidateAftershockMassReplayBytes(const TArray<uint8>& Bytes,FText& Error);
