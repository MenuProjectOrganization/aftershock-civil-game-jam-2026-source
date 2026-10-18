"""Import original instanced vehicles and shared citizen emotion atlas in UE5.8.

Idempotent graph nodes are tagged; no expression deletion or plugin changes.
Expected native body assets live in /Game/Environment/Transport.
"""
from pathlib import Path
import json
import unreal

ROOT=Path(__file__).resolve().parent.parent
SOURCE=ROOT/"_Imports"/"Original"/"Transport"
DEST="/Game/Environment/Transport"
LIB=unreal.MaterialEditingLibrary
ASSETS=unreal.EditorAssetLibrary
TOOLS=unreal.AssetToolsHelpers.get_asset_tools()


def enum_value(kind,name):
    for key in dir(kind):
        if key.replace("_","").upper()==name.replace("_","").upper():return getattr(kind,key)
    raise RuntimeError("Unreal enum unavailable: "+str(kind)+"."+name)


def import_file(filename,name,folder=DEST,options=None):
    task=unreal.AssetImportTask()
    for key,value in (("filename",str(SOURCE/filename)),("destination_path",folder),
                      ("destination_name",name),("automated",True),("replace_existing",True),("save",True)):
        task.set_editor_property(key,value)
    if options:task.set_editor_property("options",options)
    TOOLS.import_asset_tasks([task])
    value=unreal.load_asset(folder+"/"+name)
    if not value:raise RuntimeError("Import did not produce "+folder+"/"+name)
    return value


def material(name):
    path=DEST+"/Materials/"+name
    value=unreal.load_asset(path)
    if not value:
        ASSETS.make_directory(DEST+"/Materials")
        value=TOOLS.create_asset(name,DEST+"/Materials",unreal.Material,unreal.MaterialFactoryNew())
    if not isinstance(value,unreal.Material):raise RuntimeError("Expected material "+path)
    return value


def node(mat,tag,cls,**properties):
    desc="AS_TRANSPORT_V1:"+tag
    # Native material expression list is already used by project UE5.8 authoring scripts.
    existing=[x for x in LIB.get_material_expressions(mat) if str(x.get_editor_property("desc"))==desc]
    if len(existing)>1:raise RuntimeError("Duplicate tagged material expression "+desc)
    value=existing[0] if existing else LIB.create_material_expression(mat,cls,-500,0)
    value.set_editor_property("desc",desc)
    for key,val in properties.items():value.set_editor_property(key,val)
    return value


def link(source,target,input_name="",output=""):
    if not LIB.connect_material_expressions(source,output,target,input_name):
        raise RuntimeError("Cannot link "+source.get_name()+" to "+target.get_name()+"."+input_name)


def output(source,prop,port=""):
    if not LIB.connect_material_property(source,port,prop):raise RuntimeError("Cannot connect "+str(prop))


def custom(mat,tag,code,inputs,kind="CMOTFLOAT3"):
    value=node(mat,tag,unreal.MaterialExpressionCustom,code=code,
               output_type=enum_value(unreal.CustomMaterialOutputType,kind))
    fields=[]
    for name in inputs:
        field=unreal.CustomInput();field.set_editor_property("input_name",name);fields.append(field)
    value.set_editor_property("inputs",fields)
    for name,source in inputs.items():link(source,value,name)
    return value


def finish(mat):
    LIB.set_base_material_usage(mat,unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES,True)
    errors=LIB.recompile_material(mat)
    if errors:raise RuntimeError("Material compile errors "+mat.get_path_name()+": "+str(list(errors)))
    if not ASSETS.save_loaded_asset(mat):raise RuntimeError("Material did not save "+mat.get_name())
    return mat


def author_vehicle_material(palette):
    mat=material("M_VehicleBody")
    sample=node(mat,"VehiclePalette",unreal.MaterialExpressionTextureSampleParameter2D,parameter_name="VehiclePalette",texture=palette)
    uv=node(mat,"BodyUV",unreal.MaterialExpressionTextureCoordinate)
    rgb=node(mat,"VehicleRGB",unreal.MaterialExpressionPerInstanceCustomData3Vector,data_index=0,
             const_default_value=unreal.LinearColor(.25,.38,.46,1.))
    interp=node(mat,"VehicleRGBPixel",unreal.MaterialExpressionVertexInterpolator)
    link(rgb,interp)
    color=custom(mat,"PaintAndTrim","return lerp(Base,Base*max(Tint,float3(.08,.08,.08))*1.30,1-step(1.0/16.0,UV.x));",
                 {"Base":sample,"Tint":interp,"UV":uv})
    output(color,unreal.MaterialProperty.MP_BASE_COLOR)
    rough=custom(mat,"PalettePBRRoughness","int i=clamp((int)floor(UV.x*16),0,15); return i==1||i==12?.13:i==2||i==9?.86:i==7||i==14?.20:i==0?.30:.48;",{"UV":uv},"CMOTFLOAT1")
    metal=custom(mat,"PalettePBRMetallic","int i=clamp((int)floor(UV.x*16),0,15); return i==7||i==14?.9:i==0?.18:0;",{"UV":uv},"CMOTFLOAT1")
    specular=custom(mat,"PalettePBRSpecular","int i=clamp((int)floor(UV.x*16),0,15); return i==1||i==12?.70:.45;",{"UV":uv},"CMOTFLOAT1")
    lamps=custom(mat,"PaletteLamps","int i=clamp((int)floor(UV.x*16),0,15); return Base*(i==4?.30:i==5?.18:i==11?.18:0);",{"UV":uv,"Base":sample})
    output(rough,unreal.MaterialProperty.MP_ROUGHNESS);output(metal,unreal.MaterialProperty.MP_METALLIC)
    output(specular,unreal.MaterialProperty.MP_SPECULAR);output(lamps,unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    return finish(mat)


def author_rubber():
    mat=material("M_VehicleRubber")
    color=node(mat,"RubberColor",unreal.MaterialExpressionConstant3Vector,constant=unreal.LinearColor(.021,.025,.028,1))
    rough=node(mat,"RubberRoughness",unreal.MaterialExpressionConstant,r=.87)
    output(color,unreal.MaterialProperty.MP_BASE_COLOR);output(rough,unreal.MaterialProperty.MP_ROUGHNESS)
    return finish(mat)


def author_emotions(texture):
    mat=material("M_ResidentEmotions")
    mat.set_editor_property("blend_mode",unreal.BlendMode.BLEND_MASKED)
    mat.set_editor_property("shading_model",unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property("two_sided",True)
    mat.set_editor_property("opacity_mask_clip_value",.25)
    uv=node(mat,"StatusUV",unreal.MaterialExpressionTextureCoordinate)
    icon=node(mat,"StatusIndex",unreal.MaterialExpressionPerInstanceCustomData,data_index=0,const_default_value=0.)
    pixel=node(mat,"StatusIndexPixel",unreal.MaterialExpressionVertexInterpolator);link(icon,pixel)
    coordinate=custom(mat,"StatusAtlasUV","return float2((UV.x+clamp(round(Icon),0,7))/8,UV.y);",
                      {"UV":uv,"Icon":pixel},"CMOTFLOAT2")
    sample=node(mat,"StatusAtlas",unreal.MaterialExpressionTextureSampleParameter2D,parameter_name="StatusAtlas",texture=texture)
    link(coordinate,sample,"UVs")
    output(sample,unreal.MaterialProperty.MP_EMISSIVE_COLOR,"RGB")
    output(sample,unreal.MaterialProperty.MP_OPACITY_MASK,"A")
    return finish(mat)


def main():
    ASSETS.make_directory(DEST)
    palette=import_file("T_TransportPalette.png","T_TransportPalette",DEST+"/Textures")
    palette.set_editor_property("srgb",True)
    palette.set_editor_property("filter",unreal.TextureFilter.TF_NEAREST)
    palette.set_editor_property("mip_gen_settings",unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
    ASSETS.save_loaded_asset(palette)
    emoji=import_file("T_ResidentEmotions.png","T_ResidentEmotions",DEST+"/Textures")
    emoji.set_editor_property("srgb",True)
    emoji.set_editor_property("compression_settings",unreal.TextureCompressionSettings.TC_DEFAULT)
    emoji.set_editor_property("mip_gen_settings",unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
    emoji.set_editor_property("address_x",unreal.TextureAddress.TA_CLAMP);emoji.set_editor_property("address_y",unreal.TextureAddress.TA_CLAMP)
    ASSETS.save_loaded_asset(emoji)
    body=author_vehicle_material(palette);rubber=author_rubber();status=author_emotions(emoji)
    provenance=json.loads((SOURCE/"SOURCE_MANIFEST.json").read_text(encoding="utf-8"))
    expected_models={entry["name"]:entry for entry in provenance["models"]}
    report={"materials":[m.get_path_name() for m in (body,rubber,status)],"models":[],"sources":str(SOURCE/"SOURCE_MANIFEST.json"),
            "rendering":"Shared ISM materials, 80cars8buses and96statusicons maximum; no per-agent widgets/MIDs"}
    for name in ("SM_ResidentCar","SM_ResidentHatchback","SM_ResidentUtility","SM_CityMinibus","SM_VehicleWheel"):
        options=unreal.FbxImportUI()
        for key,value in (("import_mesh",True),("import_materials",False),("import_textures",False),
                          ("automated_import_should_detect_type",False),("mesh_type_to_import",unreal.FBXImportType.FBXIT_STATIC_MESH)):
            options.set_editor_property(key,value)
        data=unreal.FbxStaticMeshImportData()
        for key,value in (("combine_meshes",True),("auto_generate_collision",False),("generate_lightmap_u_vs",False),
                          ("normal_import_method",unreal.FBXNormalImportMethod.FBXNIM_IMPORT_NORMALS)):
            data.set_editor_property(key,value)
        options.set_editor_property("static_mesh_import_data",data)
        mesh=import_file(name+".obj",name,options=options)
        if not isinstance(mesh,unreal.StaticMesh):raise RuntimeError("Vehicle OBJ did not create static mesh "+name)
        for slot in range(max(1,len(mesh.get_editor_property("static_materials")))):mesh.set_material(slot,body)
        nanite=mesh.get_editor_property("nanite_settings");nanite.set_editor_property("enabled",False);mesh.set_editor_property("nanite_settings",nanite)
        if not ASSETS.save_loaded_asset(mesh):raise RuntimeError("Vehicle mesh save failed "+name)
        box=mesh.get_bounds().box_extent;dimensions=[float(box.x*2),float(box.y*2),float(box.z*2)]
        # Importer rotation or unit mismatch must not silently produce tiny traffic.
        expected=expected_models[name]["dimensions_cm"]
        # Mirrors and bumpers are included in the source vertex bounds. Test
        # against original authored geometry, never a guessed generic bus width.
        if any(abs(actual-authored)>max(1.,authored*.01) for actual,authored in zip(dimensions,expected)):
            raise RuntimeError("Vehicle import axis/scale mismatch "+name+" actual="+repr(dimensions)+" source="+repr(expected))
        report["models"].append({"asset":mesh.get_path_name(),"dimensions_cm":dimensions,"source_dimensions_cm":expected,"source_bounds_verified":True,"source_triangles":expected_models[name]["triangles"],"material":body.get_path_name(),"explicit_normals":True})
    dest=ROOT/"Saved"/"PresentationValidation"/"transport_assets.json";dest.parent.mkdir(parents=True,exist_ok=True)
    dest.write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
    unreal.log_warning("AFTERSHOCK_TRANSPORT_ASSETS_READY "+json.dumps(report))


if __name__=="__main__":main()
