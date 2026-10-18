"""Original bounded ISM fire/lava/ash/drought shaders; canonical fields own spread.

No actors per particle, simulation plugin, historical-eruption claim or graph deletion.
Run in the editor after compiling the source. Tagged nodes permit safe reruns.
"""
import unreal
import os

L=unreal.MaterialEditingLibrary
TAG="AFTERSHOCK_ECOLOGY_V1:"
ROOT="/Game/FX/Aftershock/Materials/"

def node(m,key,cls,**props):
    matches=[n for n in L.get_material_expressions(m) if str(n.get_editor_property("desc"))==TAG+key]
    if len(matches)>1: raise RuntimeError("Duplicate owned shader node "+key)
    n=matches[0] if matches else L.create_material_expression(m,cls)
    if n.get_class()!=cls.static_class(): raise RuntimeError("Owned shader node type mismatch "+key)
    n.set_editor_property("desc",TAG+key)
    for k,v in props.items(): n.set_editor_property(k,v)
    return n

def link(a,b,pin="",output=""):
    if not L.connect_material_expressions(a,output,b,pin): raise RuntimeError("Shader connection "+pin)

def out(m,n,p,channel=""):
    if not L.connect_material_property(n,channel,p): raise RuntimeError("Material output "+str(p))

def custom(m,key,code,inputs,width):
    typ=next(getattr(unreal.CustomMaterialOutputType,n) for n in dir(unreal.CustomMaterialOutputType) if n.replace("_","").upper()=="CMOTFLOAT"+str(width))
    n=node(m,key,unreal.MaterialExpressionCustom,code=code,output_type=typ)
    fields=[]
    for k in inputs:
        f=unreal.CustomInput();f.set_editor_property("input_name",k);fields.append(f)
    n.set_editor_property("inputs",fields)
    for k,v in inputs.items(): link(v,n,k)
    return n

NOISE=r"""
float2 q=P.xy*.013;
float n=sin(q.x*1.7+sin(q.y*1.3))*sin(q.y*1.9+q.x*.27);
float pulse=.75+.25*sin(TimeValue*4.+q.x+q.y);
float strength=saturate(Strength);
"""
SHADERS={
"ForestFire":r"""
float h=saturate(1.-UV.y);
float sway=sin(TimeValue*5.+h*8.+q.x)*.12*h;
float x=abs(UV.x-.5+sway);
float tongue=saturate(1.-x/(.47*(1.-h*.77)));
float flicker=.65+.35*sin(TimeValue*13.+h*20.+n*4.);
float a=tongue*smoothstep(1.,.1,h)*(.6+strength*.4)*EventEnvelope*flicker;
float3 hot=lerp(float3(6.,.35,.015),float3(9.,3.2,.15),saturate((1.-h)*1.3));
return float4(hot,a);
""",
"Lava":r"""
float flow=sin(q.x*.4+q.y*.9-TimeValue*.5)+sin(q.y*1.4+sin(q.x)-TimeValue*.35);
float molten=smoothstep(-.35,.85,flow+n*.6);
float3 crust=float3(.035,.018,.012);
float3 hot=float3(4.5,.65,.025)*(0.55+strength*.75);
return float4(lerp(crust,hot,molten*EventEnvelope),1.);
""",
"VolcanicAsh":r"""
float2 uv=UV*2.-1.;
float shape=saturate(1.-dot(uv,uv));
float billow=.65+.35*sin(q.x*.15-TimeValue*.6+sin(q.y*.2+TimeValue*.3));
return float4(float3(.095,.085,.075),shape*billow*strength*EventEnvelope*.55);
""",
"Drought":r"""
float2 w=P.xy*.016+float2(sin(q.y*.3),cos(q.x*.3))*.8;
float cracks=1.-smoothstep(.015,.09,min(abs(sin(w.x)),abs(sin(w.y))));
return float4(lerp(float3(.33,.22,.095),float3(.06,.035,.018),cracks),(.09+cracks*.22)*strength*EventEnvelope);
"""}

for kind,code in SHADERS.items():
    name="M_Disaster"+kind;path=ROOT+name
    m=unreal.load_asset(path)
    if m is None:
        unreal.EditorAssetLibrary.make_directory(ROOT.rstrip("/"))
        m=unreal.AssetToolsHelpers.get_asset_tools().create_asset(name,ROOT.rstrip("/"),unreal.Material,unreal.MaterialFactoryNew())
    if not isinstance(m,unreal.Material): raise RuntimeError("Owned path is not material "+path)
    opaque=kind=="Lava"
    m.set_editor_property("blend_mode",unreal.BlendMode.BLEND_OPAQUE if opaque else unreal.BlendMode.BLEND_TRANSLUCENT)
    m.set_editor_property("shading_model",unreal.MaterialShadingModel.MSM_UNLIT)
    m.set_editor_property("two_sided",not opaque)
    uv=node(m,"UV",unreal.MaterialExpressionTextureCoordinate)
    pos=node(m,"Position",unreal.MaterialExpressionWorldPosition)
    clock=node(m,"Time",unreal.MaterialExpressionTime)
    strength=node(m,"InstanceStrength",unreal.MaterialExpressionPerInstanceCustomData,data_index=0,const_default_value=.5)
    interp=node(m,"PixelStrength",unreal.MaterialExpressionVertexInterpolator);link(strength,interp)
    env=node(m,"Envelope",unreal.MaterialExpressionScalarParameter,parameter_name="EventEnvelope",default_value=0.)
    result=custom(m,"Shader",NOISE+code,{"P":pos,"UV":uv,"TimeValue":clock,"Strength":interp,"EventEnvelope":env},4)
    rgb=node(m,"RGB",unreal.MaterialExpressionComponentMask,r=True,g=True,b=True,a=False);link(result,rgb)
    out(m,rgb,unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    if not opaque:
        alpha=node(m,"Alpha",unreal.MaterialExpressionComponentMask,r=False,g=False,b=False,a=True);link(result,alpha)
        out(m,alpha,unreal.MaterialProperty.MP_OPACITY)
    L.set_base_material_usage(m,unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES,True)
    errors=L.recompile_material(m)
    if errors: raise RuntimeError("Material compile "+path+": "+str(errors))
    if not unreal.EditorAssetLibrary.save_loaded_asset(m): raise RuntimeError("Cannot save "+path)
    unreal.log_warning("AFTERSHOCK_ECOLOGY_SHADER_READY "+path)

for name in ("S_ForestFire_CrackleBed","S_Volcano_RoarBed","S_Drought_DryBed"):
    path="/Game/Aftershock/Audio/Generated/"+name
    sound=unreal.load_asset(path)
    if sound is None:
        source=os.path.abspath(os.path.join(unreal.Paths.project_dir(),"_Imports/Original/EnvironmentalAudio",name+".wav"))
        if not os.path.isfile(source): raise RuntimeError("Run generate_ecology_audio.py first: "+source)
        task=unreal.AssetImportTask();task.set_editor_property("filename",source)
        task.set_editor_property("destination_path","/Game/Aftershock/Audio/Generated")
        task.set_editor_property("destination_name",name);task.set_editor_property("automated",True);task.set_editor_property("save",True)
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
        sound=unreal.load_asset(path)
    if not isinstance(sound,unreal.SoundWave): raise RuntimeError("Sound import failed "+path)
    sound.set_editor_property("looping",True)
    if not unreal.EditorAssetLibrary.save_loaded_asset(sound): raise RuntimeError("Sound save failed "+path)
    unreal.log_warning("AFTERSHOCK_ORIGINAL_ECOLOGY_AUDIO_READY "+path)
