#include "AftershockIncrementalNormalRoutes.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"

FAftershockIncrementalNormalRoutes::FAftershockIncrementalNormalRoutes(uint64 InByteLimit):ByteLimit(InByteLimit)
{Jobs.Reserve(MaximumJobs);Requests.Reserve(MaximumRequests);Demand.Reserve(MaximumRequests);OrderedRequests.Reserve(MaximumRequests);}
bool FAftershockIncrementalNormalRoutes::FEntry::Earlier(int32 A,int32 B) const
{return Tree.Cost[A]<Tree.Cost[B]||(Tree.Cost[A]==Tree.Cost[B]&&A<B);}
void FAftershockIncrementalNormalRoutes::FEntry::Initialize(int32 Count,int32 Goal,const TArray<int32>& Costs)
{
    // Reserve exact bounded capacity before filling. Heap entries are UNIQUE;
    // decrease-key needs no duplicates or unbounded allocator growth in a slice.
    Tree.Cost.Reserve(Count);Tree.Next.Reserve(Count);Position.Reserve(Count);Heap.Reserve(Count);
    Tree.Cost.Init(MAX_int32,Count);Tree.Next.Init(INDEX_NONE,Count);Position.Init(INDEX_NONE,Count);
    if(Costs.IsValidIndex(Goal)&&Costs[Goal]!=MAX_int32)
    {Tree.Cost[Goal]=0;Tree.Next[Goal]=Goal;PushOrDecrease(Goal);}
}
void FAftershockIncrementalNormalRoutes::FEntry::PushOrDecrease(int32 Index)
{
    int32 At=Position[Index];if(At==-2)return;
    if(At==INDEX_NONE){At=Heap.Add(Index);Position[Index]=At;}
    while(At>0){const int32 Parent=(At-1)/2;if(!Earlier(Heap[At],Heap[Parent]))break;Heap.Swap(At,Parent);Position[Heap[At]]=At;Position[Heap[Parent]]=Parent;At=Parent;}
}
int32 FAftershockIncrementalNormalRoutes::FEntry::Pop()
{
    const int32 Index=Heap[0];const int32 Last=Heap.Last();Heap.Pop(EAllowShrinking::No);Position[Index]=-2;
    if(!Heap.IsEmpty())
    {
        Heap[0]=Last;Position[Last]=0;
        for(int32 At=0;;)
        {const int32 Left=At*2+1,Right=Left+1;int32 Best=At;
         if(Left<Heap.Num()&&Earlier(Heap[Left],Heap[Best]))Best=Left;
         if(Right<Heap.Num()&&Earlier(Heap[Right],Heap[Best]))Best=Right;
         if(Best==At)break;Heap.Swap(At,Best);Position[Heap[At]]=At;Position[Heap[Best]]=Best;At=Best;}
    }
    return Index;
}
FAftershockIncrementalNormalRoutes::EResult FAftershockIncrementalNormalRoutes::FEntry::Status(TConstArrayView<int32> Sources) const
{
    int32 Best=MAX_int32;for(int32 I:Sources)if(Position.IsValidIndex(I)&&Position[I]==-2)Best=FMath::Min(Best,Tree.Cost[I]);
    // For an unsafe source with several escape candidates, settle all equal
    // minimum-distance candidates too; existing deterministic ties stay exact.
    if(Best!=MAX_int32&&(Sources.Num()==1||Heap.IsEmpty()||Tree.Cost[Heap[0]]>Best))return EResult::Ready;
    return Heap.IsEmpty()?EResult::Unreachable:EResult::Pending;
}
int32 FAftershockIncrementalNormalRoutes::FEntry::Expand(const UTerrainGridSubsystem& Terrain,const TArray<int32>& Costs,
    const URoadNetworkSubsystem* Roads,int32 MaximumPops,double Deadline,TConstArrayView<int32> Sources)
{
    static const FIntPoint Steps[]={FIntPoint(1,0),FIntPoint(0,1),FIntPoint(-1,0),FIntPoint(0,-1)};
    int32 Pops=0;
    while(!Heap.IsEmpty()&&Pops<MaximumPops&&Status(Sources)==EResult::Pending)
    {
        if((Pops&7)==0&&FPlatformTime::Seconds()>=Deadline)break;
        const int32 Current=Pop();++Pops;const FIntPoint C=Terrain.IndexToCoord(Current);
        for(FIntPoint Step:Steps)
        {
            const FIntPoint N=C+Step;const int32 I=Terrain.CoordToIndex(N);
            if(!Costs.IsValidIndex(I)||Costs[I]==MAX_int32||!Position.IsValidIndex(I)||Position[I]==-2)continue;
            // Public geometry-only authority + frozen canonical cost mask.
            // Dynamic public-cell safety was captured by RefreshRoutingSafety;
            // actual displacement still checks the full LIVE edge predicate.
            if(Roads&&!Roads->CanTraverseWalkingProfileEdge(N,C))continue;
            const int64 Candidate=static_cast<int64>(Tree.Cost[Current])+Costs[Current];
            if(Candidate>=Tree.Cost[I]||Candidate>=MAX_int32)continue;
            Tree.Cost[I]=static_cast<int32>(Candidate);Tree.Next[I]=Current;PushOrDecrease(I);
        }
    }
    return Pops;
}
uint64 FAftershockIncrementalNormalRoutes::FEntry::Bytes() const
{return Tree.Cost.GetAllocatedSize()+Tree.Next.GetAllocatedSize()+Heap.GetAllocatedSize()+Position.GetAllocatedSize();}
uint64 FAftershockIncrementalNormalRoutes::GetAllocatedBytes() const
{
    uint64 Bytes=Jobs.GetAllocatedSize()+Requests.GetAllocatedSize()+Demand.GetAllocatedSize()+OrderedRequests.GetAllocatedSize();
    for(const auto& P:Jobs)Bytes+=P.Value.Bytes();return Bytes;
}
void FAftershockIncrementalNormalRoutes::EnsureEpoch(const UTerrainGridSubsystem& Terrain,const TArray<int32>& Costs)
{
    if(EpochRevision!=Terrain.GetTerrainRevision()||EpochCells!=Costs.Num())
    {InvalidateFields();EpochRevision=Terrain.GetTerrainRevision();EpochCells=Costs.Num();}
}
void FAftershockIncrementalNormalRoutes::InvalidateFields()
{Jobs.Reset();EpochRevision=MAX_uint64;EpochCells=0;}
void FAftershockIncrementalNormalRoutes::BeginDeliverySlice(double SecondsBudget,int32 MaximumDeliveries)
{DeliveriesLeft=FMath::Max(0,MaximumDeliveries);DeliveryCpuRemaining=FMath::Max(0.,SecondsBudget);}
double FAftershockIncrementalNormalRoutes::GetOldestPendingWallSeconds() const
{const double Now=FPlatformTime::Seconds();double Oldest=0.;for(const auto& P:Requests)Oldest=FMath::Max(Oldest,Now-P.Value.StartedAtWallSeconds);return Oldest;}
void FAftershockIncrementalNormalRoutes::RecordCompletion(uint64 Key)
{if(const auto* R=Requests.Find(Key)){++CompletedRequests;MaximumCompletedWait=FMath::Max(MaximumCompletedWait,FPlatformTime::Seconds()-R->StartedAtWallSeconds);}}
void FAftershockIncrementalNormalRoutes::Cancel(uint64 Key)
{
    if(const auto* Request=Requests.Find(Key))
    {const FIntPoint Goal=Request->Goal;Requests.Remove(Key);if(auto* Count=Demand.Find(Goal)){if(--*Count<=0)Demand.Remove(Goal);}}
}
void FAftershockIncrementalNormalRoutes::Prune(const TSet<uint64>& LiveKeys)
{TArray<uint64> Dead;for(const auto& P:Requests)if(!LiveKeys.Contains(P.Key))Dead.Add(P.Key);for(uint64 Key:Dead)Cancel(Key);}
bool FAftershockIncrementalNormalRoutes::FindRequest(uint64 Key,FVector& Destination,bool& KeepCurrent) const
{if(const auto* R=Requests.Find(Key)){Destination=R->Destination;KeepCurrent=R->KeepCurrent;return true;}return false;}
bool FAftershockIncrementalNormalRoutes::EvictUnused()
{
    FIntPoint Oldest=FIntPoint::ZeroValue;uint64 Use=MAX_uint64;bool Found=false;
    for(const auto& P:Jobs)if(Demand.FindRef(P.Key)==0&&P.Value.LastUse<Use){Oldest=P.Key;Use=P.Value.LastUse;Found=true;}
    if(Found)Jobs.Remove(Oldest);return Found;
}
FAftershockIncrementalNormalRoutes::FEntry* FAftershockIncrementalNormalRoutes::FindOrStart(FIntPoint Goal,
    const UTerrainGridSubsystem& Terrain,const TArray<int32>& Costs)
{
    if(auto* Existing=Jobs.Find(Goal))return Existing;
    const int64 Count=Costs.Num();const uint64 Needed=static_cast<uint64>(Count)*sizeof(int32)*4+64ull*1024ull;
    while((Jobs.Num()>=MaximumJobs||GetAllocatedBytes()+Needed>ByteLimit)&&EvictUnused()){}
    if(Count<=0||Count!=Terrain.Cells.Num()||Jobs.Num()>=MaximumJobs||GetAllocatedBytes()+Needed>ByteLimit)return nullptr;
    auto& Job=Jobs.Add(Goal);Job.Initialize(static_cast<int32>(Count),Terrain.CoordToIndex(Goal),Costs);Job.LastUse=++Serial;
    // Platform allocator size quantization is checked against actual retained
    // bytes. An inadmissible job is not published or counted as started.
    if(GetAllocatedBytes()>ByteLimit){Jobs.Remove(Goal);return nullptr;}++StartedJobs;
    return &Job;
}
FAftershockIncrementalNormalRoutes::EResult FAftershockIncrementalNormalRoutes::Request(uint64 Key,FIntPoint Goal,
    TConstArrayView<int32> Sources,FVector Destination,bool KeepCurrent,const UTerrainGridSubsystem& Terrain,
    const TArray<int32>& Costs,const FAftershockCitizenRouteTree*& OutTree)
{
    OutTree=nullptr;EnsureEpoch(Terrain,Costs);
    if(auto* Job=Jobs.Find(Goal))
    {
        Job->LastUse=++Serial;const EResult Result=Job->Status(Sources);
        if(Result==EResult::Unreachable){RecordCompletion(Key);Cancel(Key);return Result;}
        if(Result==EResult::Ready&&DeliveriesLeft>0&&DeliveryCpuRemaining>0.)
        {--DeliveriesLeft;OutTree=&Job->Tree;RecordCompletion(Key);Cancel(Key);return Result;}
    }
    if(!Requests.Contains(Key)&&Requests.Num()>=MaximumRequests)return EResult::Pending;
    auto* Existing=Requests.Find(Key);
    if(!Existing||Existing->Goal!=Goal){Cancel(Key);++Demand.FindOrAdd(Goal);}
    const bool IsNew=!Requests.Contains(Key);auto& R=Requests.FindOrAdd(Key);if(IsNew)R.StartedAtWallSeconds=FPlatformTime::Seconds();R.Goal=Goal;R.Sources.Reset();for(int32 I:Sources)R.Sources.Add(I);R.Destination=Destination;R.KeepCurrent=KeepCurrent;
    return EResult::Pending;
}
void FAftershockIncrementalNormalRoutes::Advance(const UTerrainGridSubsystem& Terrain,const TArray<int32>& Costs,double SecondsBudget,int32 PopBudget)
{
    const double Started=FPlatformTime::Seconds(),Deadline=Started+FMath::Max(0.,SecondsBudget);LastPops=0;EnsureEpoch(Terrain,Costs);
    OrderedRequests.Reset();for(const auto& P:Requests)OrderedRequests.Add(P.Key);OrderedRequests.Sort();
    if(OrderedRequests.IsEmpty()||PopBudget<=0){LastCpuMs=(FPlatformTime::Seconds()-Started)*1000.;return;}
    const auto* Roads=Terrain.GetWorld()?Terrain.GetWorld()->GetSubsystem<URoadNetworkSubsystem>():nullptr;
    int32 Checked=0,PassPops=0;
    while(Checked<OrderedRequests.Num()&&LastPops<PopBudget&&FPlatformTime::Seconds()<Deadline)
    {
        Cursor=Cursor%OrderedRequests.Num();const uint64 Key=OrderedRequests[Cursor++];++Checked;
        const auto* R=Requests.Find(Key);if(!R)continue;auto* Job=FindOrStart(R->Goal,Terrain,Costs);if(!Job)continue;
        if(Job->Status(R->Sources)!=EResult::Pending)continue;
        const int32 Added=Job->Expand(Terrain,Costs,Roads,FMath::Min(32,PopBudget-LastPops),Deadline,R->Sources);LastPops+=Added;PassPops+=Added;
        // Revisit runnable requests round-robin within the SAME bounded slice.
        if(Checked==OrderedRequests.Num()&&PassPops>0&&LastPops<PopBudget&&FPlatformTime::Seconds()<Deadline){Checked=0;PassPops=0;}
    }
    LastCpuMs=(FPlatformTime::Seconds()-Started)*1000.;
}
