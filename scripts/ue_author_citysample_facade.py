"""City Sample facade integration candidate. Execute ONLY in the real game project.

Requires parent's dependency-safe migration first. No download or file copying.
Consumes the parent's bounded native extracted textures and wraps existing masters.
Every original damage/power/retrofit/WPO chain remains connected. Default strength
is zero; parent binds actual visual families on shared building-type MIDs.
Run last, after original architecture material authoring. No expression deletion.
"""
from pathlib import Path
import json
import unreal

L=unreal.MaterialEditingLibrary
A=unreal.EditorAssetLibrary
TAG="AFTERSHOCK_CITYSAMPLE_FACADE_V1:"
DEST="/Game/Aftershock/CitySample/Textures"
BRICK_ROOT="/Game/Prop/Kit_roof_brick_RR"
SOURCE_BRICK_MATERIAL=BRICK_ROOT+"/Material/M_roof_brick"
FAMILIES={
    "Brick":{"C":DEST+"/T_Brick_C","N":DEST+"/T_Brick_N","M":DEST+"/T_Brick_M"},
    "Concrete":{"C":DEST+"/T_Concrete_C","N":DEST+"/T_Concrete_N"},
}
SOURCE_PACKED_TEXTURE=BRICK_ROOT+"/Texture/T_roof_bricks_M"
MASTERS=("/Game/Buildings/Materials/M_Building_Base","/Game/Buildings/Materials/M_ServiceBuilding")


def enum_value(cls,name):
    for candidate in dir(cls):
        if candidate.replace("_","").upper()==name.replace("_","").upper():return getattr(cls,candidate)
    raise RuntimeError("Missing native enum "+name)


def conn(material,prop):
    return L.get_material_property_input_node(material,prop),str(L.get_material_property_input_node_output_name(material,prop))


def input_conn(material,expression,pin):
    names=list(L.get_material_expression_input_names(expression))
    values=list(L.get_inputs_for_material_expression(material,expression))
    if pin not in names or names.index(pin)>=len(values) or values[names.index(pin)] is None:
        raise RuntimeError("Cannot read preserved input "+pin)
    source=values[names.index(pin)]
    output=L.get_input_node_output_name_for_material_expression(expression,source)
    if isinstance(output,(tuple,list)):
        if output and isinstance(output[0],bool) and not output[0]:raise RuntimeError("Native output pin unavailable")
        strings=[x for x in output if isinstance(x,str)]
        if len(strings)!=1:raise RuntimeError("Ambiguous native output pin")
        output=strings[0]
    if not isinstance(output,str):raise RuntimeError("Unrecognized output pin readback")
    return source,output


def native_source_channel_proof():
    """Trace native source graph edges; never infer channel order from '_M'."""
    instance=unreal.load_asset(SOURCE_BRICK_MATERIAL) if A.does_asset_exist(SOURCE_BRICK_MATERIAL) else None
    if not instance:return {"proven":False,"reason":"Source brick graph not present in bounded extraction; keep existing roughness","source_material":SOURCE_BRICK_MATERIAL}
    material=instance
    while isinstance(material,unreal.MaterialInstanceConstant):
        material=material.get_editor_property("parent")
    if not isinstance(material,unreal.Material):raise RuntimeError("Source brick has no native parent material")
    target=SOURCE_PACKED_TEXTURE
    trace=[]
    def leaf_texture(expression):
        cls=expression.get_class().get_name()
        if not cls.startswith("MaterialExpressionTextureSample"):return None
        texture=expression.get_editor_property("texture")
        if cls=="MaterialExpressionTextureSampleParameter2D" and isinstance(instance,unreal.MaterialInstanceConstant):
            override=L.get_material_instance_texture_parameter_value(instance,expression.get_editor_property("parameter_name"))
            if override:texture=override
        return texture
    def walk(expression,port,seen):
        if not expression or expression.get_path_name() in seen:return set()
        seen=seen|{expression.get_path_name()}
        cls=expression.get_class().get_name()
        texture=leaf_texture(expression)
        if texture:
            if texture.get_path_name().split(".")[0]!=target:return set()
            channel=port.upper()
            if channel in ("R","G","B","A"):return {channel}
            # A vector connection is not proof of a scalar packed channel.
            return {"AMBIGUOUS_"+(channel or "RGB")}
        trace.append({"node":expression.get_path_name(),"class":cls,"output":port})
        if cls=="MaterialExpressionComponentMask":
            selected=[x.upper() for x in ("r","g","b","a") if expression.get_editor_property(x)]
            if len(selected)!=1:return {"AMBIGUOUS_COMPONENT_MASK"}
            names=list(L.get_material_expression_input_names(expression))
            if not names:return set()
            child,child_port=input_conn(material,expression,names[0])
            return walk(child,child_port if child_port.upper() in ("R","G","B","A") else selected[0],seen)
        # Inverted/gloss-style data needs a separately proven conversion;
        # source-channel identity alone must not enable raw roughness sampling.
        if cls in ("MaterialExpressionOneMinus","MaterialExpressionSubtract"):
            return {"UNSUPPORTED_ROUGHNESS_INVERSION"}
        # Scalar arithmetic may scale one source channel. Repeated use of one
        # expression on two input pins is ambiguous in this reflected API,
        # which returns an output channel by source node rather than pin.
        linked=[n.get_path_name() for n in L.get_inputs_for_material_expression(material,expression) if n]
        if len(set(linked))!=len(linked):return {"AMBIGUOUS_SHARED_INPUT_EDGES"}
        # Do not collapse a vector/function/attribute graph into a guess.
        permitted=("MaterialExpressionMultiply","MaterialExpressionAdd","MaterialExpressionSubtract",
                   "MaterialExpressionDivide","MaterialExpressionOneMinus","MaterialExpressionClamp",
                   "MaterialExpressionSaturate","MaterialExpressionPower","MaterialExpressionLinearInterpolate")
        if cls not in permitted:return set()
        result=set()
        for name in L.get_material_expression_input_names(expression):
            try:child,child_port=input_conn(material,expression,name)
            except RuntimeError:continue
            result|=walk(child,child_port,seen)
        return result
    expression,port=conn(material,unreal.MaterialProperty.MP_ROUGHNESS)
    channels=walk(expression,port,set())
    proof={"source_material":instance.get_path_name(),"native_parent":material.get_path_name(),
           "target_packed_texture":target,"roughness_source_channels":sorted(channels),"trace":trace,
           "proof_method":"Native material property and expression output-channel connections, not filename"}
    if len(channels)!=1 or next(iter(channels)) not in "RGBA":
        proof["proven"]=False
        return proof
    proof.update(proven=True,roughness_channel=next(iter(channels)),
                 roughness_channel_index="RGBA".index(next(iter(channels))))
    return proof


def extracted_texture_readback():
    """Read only: parent's native extraction owns textures and provenance."""
    output={}
    for family,sources in FAMILIES.items():
        output[family]={}
        for role,path in sources.items():
            texture=unreal.load_asset(path)
            if not isinstance(texture,unreal.Texture2D):raise RuntimeError("Missing native extracted Texture2D "+path)
            if texture.get_editor_property("virtual_texture_streaming"):raise RuntimeError("Facade custom sample needs nonvirtual extracted texture "+path)
            if bool(texture.get_editor_property("srgb"))!=(role=="C"):raise RuntimeError("Extracted texture sRGB policy mismatch "+path)
            compression=texture.get_editor_property("compression_settings")
            expected=enum_value(unreal.TextureCompressionSettings,"TCNORMALMAP" if role=="N" else "TCMASKS" if role=="M" else "TCDEFAULT")
            if compression!=expected:raise RuntimeError("Extracted compression/sampler mismatch "+path+" "+str(compression))
            max_size=int(texture.get_editor_property("max_texture_size"))
            if max_size<=0 or max_size>2048:raise RuntimeError("Extracted texture must have explicit max2048 cap "+path)
            output[family][role]={"derived":texture.get_path_name(),
                "native_dimensions":[texture.blueprint_get_size_x(),texture.blueprint_get_size_y()],
                "max_texture_size":max_size,"srgb":role=="C","virtual_texture_streaming":False,
                "source_bulk_downsample_claim":False,"provenance_owner":"parent native extraction manifest"}
    return output

def node(material,label,cls,**values):
    matching=[x for x in L.get_material_expressions(material) if str(x.get_editor_property("desc"))==TAG+label]
    if len(matching)>1:raise RuntimeError("Duplicate facade graph tag "+label)
    value=matching[0] if matching else L.create_material_expression(material,cls)
    if value.get_class()!=cls.static_class():raise RuntimeError("Facade node class mismatch "+label)
    value.set_editor_property("desc",TAG+label)
    for key,field in values.items():value.set_editor_property(key,field)
    return value


def link(source,target,pin="",port=""):
    if isinstance(source,tuple):source,port=source
    if not L.connect_material_expressions(source,port,target,pin):raise RuntimeError("Facade link failed "+pin)


def custom(material,label,code,inputs,width):
    value=node(material,label,unreal.MaterialExpressionCustom,code=code,
               output_type=enum_value(unreal.CustomMaterialOutputType,"CMOTFLOAT"+str(width)))
    fields=[]
    for name in inputs:
        field=unreal.CustomInput();field.set_editor_property("input_name",name);fields.append(field)
    value.set_editor_property("inputs",fields)
    for name,source in inputs.items():link(source,value,name)
    return value


def scalar(material,name,default):
    return node(material,name,unreal.MaterialExpressionScalarParameter,parameter_name=name,default_value=float(default))


def originals(material,label,prop):
    expression,port=conn(material,prop)
    if expression and str(expression.get_editor_property("desc"))==TAG+label:
        return input_conn(material,expression,"Original")
    if not expression:raise RuntimeError("Existing material output missing "+label)
    # Detect a later authoring pass around our existing wrapper, which would
    # create a cycle on reuse. This script intentionally runs last.
    if any(str(n.get_editor_property("desc"))==TAG+label for n in L.get_material_expressions(material)):
        raise RuntimeError("Facade wrapper is no longer outermost; preserve and review authoring order")
    return expression,port


def cpd_snapshot(material):
    result=[]
    for expr in L.get_material_expressions(material):
        cls=expr.get_class().get_name()
        if cls.startswith("MaterialExpressionPerInstanceCustomData"):
            result.append((expr.get_path_name(),cls,int(expr.get_editor_property("data_index"))))
        elif cls=="MaterialExpressionScalarParameter" and expr.get_editor_property("use_custom_primitive_data"):
            result.append((expr.get_path_name(),"LegacyCPD",int(expr.get_editor_property("primitive_data_index"))))
    return sorted(result)


def author(path,texture_info,proof):
    material=unreal.load_asset(path)
    if not isinstance(material,unreal.Material):raise RuntimeError("Existing shared building master missing "+path)
    if not material.get_editor_property("tangent_space_normal"):raise RuntimeError("Unknown existing world-space normal contract")
    preserved={name:conn(material,prop) for name,prop in (
        ("Emissive",unreal.MaterialProperty.MP_EMISSIVE_COLOR),("WPO",unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET),
        ("Metallic",unreal.MaterialProperty.MP_METALLIC),("Opacity",unreal.MaterialProperty.MP_OPACITY),
        ("OpacityMask",unreal.MaterialProperty.MP_OPACITY_MASK),("AO",unreal.MaterialProperty.MP_AMBIENT_OCCLUSION))}
    data_before=cpd_snapshot(material)
    old_base=originals(material,"FacadeBase",unreal.MaterialProperty.MP_BASE_COLOR)
    old_normal=originals(material,"FacadeNormal",unreal.MaterialProperty.MP_NORMAL)
    old_rough=originals(material,"FacadeRoughness",unreal.MaterialProperty.MP_ROUGHNESS)
    palettes=[n for n in L.get_material_expressions(material)
              if n.get_class().get_name()=="MaterialExpressionTextureSampleParameter2D"
              and str(n.get_editor_property("parameter_name")) in ("BuildingAlbedo","ServicePalette")]
    if len(palettes)!=1:raise RuntimeError("Cannot prove original UV0 source palette")
    source_palette=palettes[0]
    position=node(material,"WorldPosition",unreal.MaterialExpressionWorldPosition,
                  world_position_shader_offset=enum_value(unreal.WorldPositionIncludedOffsets,"WPTEXCLUDEALLSHADEROFFSETS"))
    normal=node(material,"GeometryNormal",unreal.MaterialExpressionVertexNormalWS)
    camera=node(material,"CameraPosition",unreal.MaterialExpressionCameraPositionWS)
    strength=scalar(material,"CitySampleFacadeStrength",0.)
    tile=scalar(material,"CitySampleFacadeTileCm",200.)
    fade_start=scalar(material,"CitySampleFacadeFadeStartCm",20000.)
    fade_end=scalar(material,"CitySampleFacadeFadeEndCm",60000.)
    normal_strength=scalar(material,"CitySampleFacadeNormalStrength",.45)
    rough_enable=scalar(material,"CitySampleFacadeUsePackedRoughness",1. if proof["proven"] else 0.)
    rough_channel=scalar(material,"CitySampleFacadePackedRoughnessChannel",proof.get("roughness_channel_index",0))
    texture_nodes={}
    for role,name,sampler in (("C","Albedo","SAMPLERTYPECOLOR"),("N","Normal","SAMPLERTYPENORMAL"),("M","Packed","SAMPLERTYPEMASKS")):
        tex=unreal.load_asset(texture_info["Brick"][role]["derived"])
        texture_nodes[role]=node(material,name+"Object",unreal.MaterialExpressionTextureObjectParameter,
            parameter_name="CitySampleFacade"+name,texture=tex,sampler_type=enum_value(unreal.MaterialSamplerType,sampler))
    uv=custom(material,"DominantAxisUV",r"""
float3 ng=normalize(N);float3 q=P/max(Tile,25);
if(abs(ng.z)>.7)return q.xy;
return abs(ng.x)>=abs(ng.y)?float2(q.y*(ng.x<0?-1:1),q.z):float2(q.x*(ng.y<0?1:-1),q.z);
""",{"P":position,"N":normal,"Tile":tile},2)
    weight=custom(material,"FacadeWeight",r"""
float glass=step(Palette.r*1.4,Palette.b)*step(Palette.r*1.25,Palette.g)*(1-step(.20,Palette.r));
float walls=1-smoothstep(.55,.80,abs(normalize(N).z));
float nearFade=1-smoothstep(Start,max(Start+100,End),distance(P,Camera));
return saturate(Strength)*walls*(1-glass)*nearFade;
""",{"Palette":source_palette,"N":normal,"Strength":strength,"P":position,"Camera":camera,"Start":fade_start,"End":fade_end},1)
    base=custom(material,"FacadeBase",r"""
float2 dx=ddx(UV),dy=ddy(UV);
[branch]if(Weight<=.0001)return Original;
float3 photo=Texture2DSampleGrad(Facade,FacadeSampler,UV,dx,dy).rgb;
// A bounded multiplicative photograph preserves the entire upstream canonical
// damage, cracks, chips and retrofit tint; no bright replacement fills cracks.
float3 modulation=clamp(photo/.35,.32,1.80);
return lerp(Original,saturate(Original*modulation),Weight);
""",{"Original":old_base,"UV":uv,"Weight":weight,"Facade":texture_nodes["C"]},3)
    detail_world=custom(material,"ProjectedPhotoNormalWS",r"""
float3 ng=normalize(N);float2 dx=ddx(UV),dy=ddy(UV);
[branch]if(Weight<=.0001)return ng;
float2 xy=Texture2DSampleGrad(Detail,DetailSampler,UV,dx,dy).rg*2-1;
float z=sqrt(saturate(1-dot(xy,xy)));
if(abs(ng.x)>=abs(ng.y)){float sx=ng.x<0?-1:1;return normalize(float3(sx*z,sx*xy.x,xy.y));}
float sy=ng.y<0?-1:1;return normalize(float3(-sy*xy.x,sy*z,xy.y));
""",{"N":normal,"UV":uv,"Weight":weight,"Detail":texture_nodes["N"]},3)
    tangent=node(material,"WorldPhotoToExistingTangent",unreal.MaterialExpressionTransform,
        transform_source_type=enum_value(unreal.MaterialVectorCoordTransformSource,"TRANSFORMSOURCEWORLD"),
        transform_type=enum_value(unreal.MaterialVectorCoordTransform,"TRANSFORMTANGENT"))
    link(detail_world,tangent)
    final_normal=custom(material,"FacadeNormal",r"""
float amount=saturate(Strength)*Weight;
float3 photo=normalize(lerp(float3(0,0,1),Photo,amount));
return normalize(float3(Original.xy+photo.xy,Original.z*photo.z));
""",{"Original":old_normal,"Photo":tangent,"Weight":weight,"Strength":normal_strength},3)
    rough=custom(material,"FacadeRoughness",r"""
float2 dx=ddx(UV),dy=ddy(UV);
[branch]if(Weight<=.0001||UsePacked<.5)return Original;
float4 packedSample=Texture2DSampleGrad(Packed,PackedSampler,UV,dx,dy);
int channel=clamp((int)round(Channel),0,3);
return lerp(Original,clamp(packedSample[channel],.45,.95),Weight);
""",{"Original":old_rough,"UV":uv,"Weight":weight,"UsePacked":rough_enable,"Channel":rough_channel,"Packed":texture_nodes["M"]},1)
    for expression,prop in ((base,unreal.MaterialProperty.MP_BASE_COLOR),(final_normal,unreal.MaterialProperty.MP_NORMAL),(rough,unreal.MaterialProperty.MP_ROUGHNESS)):
        if not L.connect_material_property(expression,"",prop):raise RuntimeError("Facade output connection failed")
    for name,prop in (("Emissive",unreal.MaterialProperty.MP_EMISSIVE_COLOR),("WPO",unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET),
                      ("Metallic",unreal.MaterialProperty.MP_METALLIC),("Opacity",unreal.MaterialProperty.MP_OPACITY),
                      ("OpacityMask",unreal.MaterialProperty.MP_OPACITY_MASK),("AO",unreal.MaterialProperty.MP_AMBIENT_OCCLUSION)):
        if conn(material,prop)!=preserved[name]:raise RuntimeError("Canonical/nonfacade output changed "+name)
    if cpd_snapshot(material)!=data_before:raise RuntimeError("Canonical instance data reader layout changed")
    for expression,old in ((base,old_base),(final_normal,old_normal),(rough,old_rough)):
        if input_conn(material,expression,"Original")!=old:raise RuntimeError("Existing graph disconnected")
    errors=L.recompile_material(material)
    if errors:raise RuntimeError("Facade compile request errors "+str(list(errors)))
    return material,{"material":path,"preserved_outputs":{k:{"node":v[0].get_path_name() if v[0] else None,"output":v[1]} for k,v in preserved.items()},
        "old_base_upstream":old_base[0].get_path_name(),"old_normal_upstream":old_normal[0].get_path_name(),
        "old_roughness_upstream":old_rough[0].get_path_name(),"instance_data_before_after_equal":True,
        "default_strength":0.,"dominant_axis_texture_samples_per_enabled_fragment":3,"glass_and_roof_excluded":True,
        "near_fade_cm":[20000,60000],"compile_request_errors":[]}


def main():
    # A Content junction into Vault is suitable for read-only source probing,
    # but must never receive duplicate/material authoring writes.
    content=Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_content_dir())).resolve()
    if "vaultcache" in str(content).lower():raise RuntimeError("Refuse writing through the source VaultCache Content junction")
    saved=Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir())).resolve()/"CitySampleAudit"
    saved.mkdir(parents=True,exist_ok=True)
    proof=native_source_channel_proof()
    recorded=saved/"extracted_brick_channel_proof.json"
    if not proof["proven"] and recorded.is_file():
        candidate=json.loads(recorded.read_text(encoding="utf-8"))
        if candidate.get("proven") and candidate.get("target_packed_texture")==SOURCE_PACKED_TEXTURE and candidate.get("roughness_channel") in ("R","G","B","A"):
            candidate["roughness_channel_index"]="RGBA".index(candidate["roughness_channel"])
            proof=candidate
    (saved/"facade_source_channel_readback.json").write_text(json.dumps(proof,indent=2)+"\n",encoding="utf-8")
    info=extracted_texture_readback()
    built=[author(path,info,proof) for path in MASTERS]
    for material,_ in built:
        if not A.save_loaded_asset(material):raise RuntimeError("Facade master save failed")
    report={"source":"Epic Games City Sample; private project collaborator use under applicable acquisition license",
        "license_urls":["https://www.fab.com/listings/4898e707-7855-404b-af0e-a505ee690e68","https://www.unrealengine.com/eula/content","https://www.fab.com/eula"],
        "textures":info,"brick_packed_roughness_native_proof":proof,"materials":[record for _,record in built],
        "family_binding":{"Brick":{"UsePackedRoughness":1 if proof["proven"] else 0,"Channel":proof.get("roughness_channel_index"),"reason":"Native source graph proof required; original roughness retained when unavailable"},
                          "Concrete":{"UsePackedRoughness":0,"reason":"No concrete packed-channel proof; preserve original roughness until native graph readback"}},
        "binding_parameters":["CitySampleFacadeAlbedo","CitySampleFacadeNormal","CitySampleFacadePacked","CitySampleFacadeStrength","CitySampleFacadeUsePackedRoughness","CitySampleFacadePackedRoughnessChannel"],
        "no_asset_download":True,"no_texture_import_or_duplication":True,"no_gameplay_changes":True,"no_vault_source_writes":True,"source_bulk_size_not_reduced_by_max_texture_size":True,
        "proof_scope":"Native source graph channel trace when available, extracted texture readback, output preservation, compile request and save. Live render/resource/performance acceptance remains separate."}
    (saved/"facade_authoring_report.json").write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
    unreal.log_warning("AFTERSHOCK_CITYSAMPLE_FACADE_READY "+json.dumps(report))


if __name__=="__main__":main()
