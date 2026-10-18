#pragma once
#include "CoreMinimal.h"

/** Deterministic numerical support preparation, not a predicted runout extent.
 * Retains every resident cell (including a separately mapped volcanic vent),
 * requests at most 9x9 complete 25-cell chunks around the ORIGINAL city frame,
 * and never extends beyond actual source-covered cell centres. */
namespace AftershockMassSupportPreparation
{
struct FPlan
{
    FIntRect RequestedCells=FIntRect(0,0,0,0);
    TArray<FIntPoint> MissingChunks;
    int32 ExistingCells=0,AdditionalCells=0,FinalCells=0;
    bool bClippedToSource=false;
};
inline int64 FloorDiv(int64 Value,int64 Divisor)
{return Value/Divisor-(Value%Divisor<0?1:0);}
inline bool BuildPlan(FIntPoint OriginalSize,const FIntRect& SourceBounds,
    TConstArrayView<FIntPoint> ResidentCoords,FPlan& Out,FString& Error)
{
    Error.Reset();constexpr int32 Chunk=25,Width=225,Limit=65536,Abs=1000000;
    if(OriginalSize.X<=0||OriginalSize.Y<=0||OriginalSize.X>Limit||OriginalSize.Y>Limit
        ||ResidentCoords.IsEmpty()||ResidentCoords.Num()>Limit
        ||static_cast<int64>(SourceBounds.Max.X)-SourceBounds.Min.X<=0
        ||static_cast<int64>(SourceBounds.Max.Y)-SourceBounds.Min.Y<=0)
    {Error=TEXT("Mass support needs a nonempty bounded original frame and real-source extent.");return false;}
    TSet<FIntPoint> Existing;Existing.Reserve(ResidentCoords.Num());
    for(FIntPoint C:ResidentCoords)
    {
        if(FMath::Abs(static_cast<int64>(C.X))>Abs||FMath::Abs(static_cast<int64>(C.Y))>Abs||Existing.Contains(C))
        {Error=TEXT("Resident support contains duplicate or out-of-range coordinates.");return false;}Existing.Add(C);
    }
    const auto FirstFull=[&](int32 V){return -FloorDiv(-static_cast<int64>(V),Chunk);};
    const int64 FullMinX=FirstFull(FMath::Max(SourceBounds.Min.X,-Abs)),FullMinY=FirstFull(FMath::Max(SourceBounds.Min.Y,-Abs));
    const int64 FullMaxX=FloorDiv(FMath::Min(SourceBounds.Max.X,Abs+1),Chunk),FullMaxY=FloorDiv(FMath::Min(SourceBounds.Max.Y,Abs+1),Chunk);
    const int64 DesiredMinX=FloorDiv(FloorDiv(static_cast<int64>(OriginalSize.X)-Width,2),Chunk);
    const int64 DesiredMinY=FloorDiv(FloorDiv(static_cast<int64>(OriginalSize.Y)-Width,2),Chunk);
    const int64 MinX=FMath::Max(DesiredMinX,FullMinX),MinY=FMath::Max(DesiredMinY,FullMinY);
    const int64 MaxX=FMath::Min(DesiredMinX+9,FullMaxX),MaxY=FMath::Min(DesiredMinY+9,FullMaxY);
    if(MinX>=MaxX||MinY>=MaxY){Error=TEXT("No complete source-covered chunk supports this preparation window.");return false;}
    FPlan Candidate;Candidate.ExistingCells=ResidentCoords.Num();
    Candidate.RequestedCells=FIntRect(FIntPoint(static_cast<int32>(MinX*Chunk),static_cast<int32>(MinY*Chunk)),
        FIntPoint(static_cast<int32>(MaxX*Chunk),static_cast<int32>(MaxY*Chunk)));
    Candidate.bClippedToSource=MinX!=DesiredMinX||MinY!=DesiredMinY||MaxX!=DesiredMinX+9||MaxY!=DesiredMinY+9;
    for(int64 CY=MinY;CY<MaxY;++CY)for(int64 CX=MinX;CX<MaxX;++CX)
    {
        int32 Missing=0;
        for(int32 Y=0;Y<Chunk;++Y)for(int32 X=0;X<Chunk;++X)
            Missing+=!Existing.Contains(FIntPoint(static_cast<int32>(CX*Chunk+X),static_cast<int32>(CY*Chunk+Y)));
        if(Missing>0){Candidate.MissingChunks.Add(FIntPoint(static_cast<int32>(CX),static_cast<int32>(CY)));Candidate.AdditionalCells+=Missing;}
    }
    Candidate.FinalCells=Candidate.ExistingCells+Candidate.AdditionalCells;
    if(Candidate.FinalCells>Limit){Error=TEXT("Prepared support would exceed 65,536 resident cells; no cells added.");return false;}
    Out=MoveTemp(Candidate);return true;
}
}
