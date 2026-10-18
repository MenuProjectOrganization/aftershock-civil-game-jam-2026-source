#pragma once
#include "CoreMinimal.h"
#include "Aftershock/Public/CrowdRenderSubsystem.h"
class UTerrainGridSubsystem;
class URoadNetworkSubsystem;

/** Game-thread derived search only. No UObject/pointer retained across slices. */
class FAftershockIncrementalNormalRoutes
{
public:
    static constexpr int32 MaximumJobs=128,MaximumRequests=1500;
    enum class EResult:uint8{Pending,Ready,Unreachable};
    explicit FAftershockIncrementalNormalRoutes(uint64 InByteLimit);
    EResult Request(uint64 Key,FIntPoint Goal,TConstArrayView<int32> Sources,FVector Destination,bool KeepCurrent,
        const UTerrainGridSubsystem& Terrain,const TArray<int32>& Costs,const FAftershockCitizenRouteTree*& OutTree);
    /** Delivery is independently capped so many completed routes cannot fan out
     * into an unbounded callback after a cheap shared search slice. */
    void BeginDeliverySlice(double SecondsBudget=0.002,int32 MaximumDeliveries=16);
    void EndDeliverySlice(){DeliveriesLeft=0;DeliveryCpuRemaining=0.;}
    void ConsumeDeliveryCpu(double Seconds){DeliveryCpuRemaining=FMath::Max(0.,DeliveryCpuRemaining-FMath::Max(0.,Seconds));}
    bool FindRequest(uint64 Key,FVector& Destination,bool& KeepCurrent) const;
    bool HasRequest(uint64 Key) const{return Requests.Contains(Key);}
    void Cancel(uint64 Key);
    void Prune(const TSet<uint64>& LiveKeys);
    void InvalidateFields();
    /** Shared complete callback slice; indexed decrease-key heap has <=one entry/cell. */
    void Advance(const UTerrainGridSubsystem& Terrain,const TArray<int32>& Costs,double SecondsBudget=0.002,int32 PopBudget=4096);
    uint64 GetAllocatedBytes() const;
    int32 GetJobCount() const{return Jobs.Num();}
    int32 GetRequestCount() const{return Requests.Num();}
    uint64 GetStartedJobs() const{return StartedJobs;}
    int32 GetLastPops() const{return LastPops;}
    double GetLastCpuMs() const{return LastCpuMs;}
    double GetOldestPendingWallSeconds() const;
    double GetMaximumCompletedWaitSeconds() const{return MaximumCompletedWait;}
    uint64 GetCompletedRequests() const{return CompletedRequests;}
private:
    struct FEntry
    {
        FAftershockCitizenRouteTree Tree;
        TArray<int32> Heap,Position; // Position:-1 unseen,-2 finalized,>=0 heap slot.
        uint64 LastUse=0;
        void Initialize(int32 Count,int32 Goal,const TArray<int32>& Costs);
        void PushOrDecrease(int32 Index);
        int32 Pop();
        bool Earlier(int32 A,int32 B) const;
        EResult Status(TConstArrayView<int32> Sources) const;
        int32 Expand(const UTerrainGridSubsystem& Terrain,const TArray<int32>& Costs,const URoadNetworkSubsystem* Roads,
            int32 MaximumPops,double Deadline,TConstArrayView<int32> Sources);
        uint64 Bytes() const;
    };
    struct FRequest
    {
        FIntPoint Goal;TArray<int32,TInlineAllocator<4>> Sources;
        FVector Destination=FVector::ZeroVector;bool KeepCurrent=false;double StartedAtWallSeconds=0.;
    };
    TMap<FIntPoint,FEntry> Jobs;
    TMap<uint64,FRequest> Requests;
    TMap<FIntPoint,int32> Demand;
    TArray<uint64> OrderedRequests;
    uint64 ByteLimit=0,Serial=0,StartedJobs=0,EpochRevision=MAX_uint64,CompletedRequests=0;
    int32 EpochCells=0,LastPops=0,Cursor=0,DeliveriesLeft=0;double LastCpuMs=0.,DeliveryCpuRemaining=0.,MaximumCompletedWait=0.;
    void EnsureEpoch(const UTerrainGridSubsystem& Terrain,const TArray<int32>& Costs);
    FEntry* FindOrStart(FIntPoint Goal,const UTerrainGridSubsystem& Terrain,const TArray<int32>& Costs);
    bool EvictUnused();
    void RecordCompletion(uint64 Key);
};
