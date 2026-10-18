"""Native shared ground refinement; run through coordinated Unreal authoring.

Original graph refinement. Reuses exactly ONE additional existing CC0 PBR set;
no new downloads, imagery, masks, geometry, WPO, physical state or save fields.
Texture samplers are shared through the same UE5.8 helper emitted by native
material translation. Compiled resource statistics MUST show <=16 samplers.
This script does not launch Unreal or perform a gameplay/profile assertion.
"""
from pathlib import Path
import json
import re
import unreal

ROOT=Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())).resolve()
L=unreal.MaterialEditingLibrary
A=unreal.EditorAssetLibrary
STYLE='AFTERSHOCK_REGIONAL_STYLE_V1:'
GLOBAL='AFTERSHOCK_GLOBAL_REGIONAL_COLOUR_V1:'
TAG='AS_GROUND_REFINEMENT_V1:'
PATHS=('/Game/Environment/Materials/Presentation/M_PresentationGroundPBR',
       '/Game/Environment/Materials/Presentation/M_AftershockRegionalBlend')
PROTECTED=(unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET,unreal.MaterialProperty.MP_OPACITY,
           unreal.MaterialProperty.MP_OPACITY_MASK,unreal.MaterialProperty.MP_METALLIC,
           unreal.MaterialProperty.MP_EMISSIVE_COLOR)

def connection(m,p):
    n=L.get_material_property_input_node(m,p)
    return (n,str(L.get_material_property_input_node_output_name(m,p)))

def tagged(m,description):
    nodes=[n for n in L.get_material_expressions(m) if str(n.get_editor_property('desc'))==description]
    if len(nodes)!=1:raise RuntimeError('Missing/ambiguous owned expression '+description)
    return nodes[0]

def authored(m,kind,name,**props):
    matches=[n for n in L.get_material_expressions(m) if str(n.get_editor_property('desc'))==TAG+name]
    if len(matches)>1:raise RuntimeError('Duplicate refinement expression '+name)
    n=matches[0] if matches else L.create_material_expression(m,getattr(unreal,'MaterialExpression'+kind))
    if n.get_class()!=getattr(unreal,'MaterialExpression'+kind).static_class():raise RuntimeError('Refinement expression type '+name)
    n.set_editor_property('desc',TAG+name)
    for key,value in props.items():n.set_editor_property(key,value)
    return n

def custom_input(expression,name,source):
    inputs=list(expression.get_editor_property('inputs'))
    matches=[i for i in inputs if str(i.get_editor_property('input_name'))==name]
    if len(matches)>1:raise RuntimeError('Duplicate custom input '+name)
    if not matches:
        item=unreal.CustomInput();item.set_editor_property('input_name',name);inputs.append(item)
        expression.set_editor_property('inputs',inputs)
    if not L.connect_material_expressions(source,'',expression,name):raise RuntimeError('Refinement input '+name)

def replace_once(code,before,after):
    if code.count(before)!=1:raise RuntimeError('Owned shader anchor changed: '+before[:90])
    return code.replace(before,after,1)

# All three roles share exactly this same continuous, non-cellular domain and
# source-mask weight. No random rotation/reset per square tile or per grid cell.
REFINE=r'''
// AS_GROUND_REFINEMENT_V1:COMMON
float3 qa=P/max(AccentTile,50.);
qa+=(float3(domain.noise(qa.xy*.23+11.7),domain.noise(qa.yz*.19+37.3),domain.noise(qa.zx*.27+71.9))-.5)*1.35;
float accentNeed=BiomeSlot<.5 ? (.25+.45*(1.-saturate(Wetness)))*(1.-forest)*(1.-agriculture*.3)
               : BiomeSlot<1.5 ? (.12+.6*saturate(Wetness))*(1.-agriculture)*(.35+.65*forest)
               : BiomeSlot<2.5 ? (1.-forest)
               : saturate(agriculture*.6+forest*.12);
float accentWeight=saturate(Refinement)*saturate(accentNeed)*saturate(1.-rock)
                   *lerp(.3,1.,smoothstep(.28,.72,macroVariation));
'''

def share_pbr_samplers(code):
    # Material.Wrap_WorldGroupSettings is an always-declared sampler member in
    # UE5.8 MaterialUniformExpressions.cpp960. Unlike a raw View bilinear sampler
    # it preserves the world's sampler settings. Image/mask samples keep CLAMP.
    for texture in ('Base','Rock','Accent'):
        code=code.replace('Texture2DSample('+texture+','+texture+'Sampler,',
                          'Texture2DSample('+texture+',GetMaterialSharedSampler('+texture+'Sampler,Material.Wrap_WorldGroupSettings),')
    return code

def refine(m):
    before={p:connection(m,p) for p in PROTECTED}
    outputs={p:connection(m,p) for p in (unreal.MaterialProperty.MP_BASE_COLOR,
                                        unreal.MaterialProperty.MP_NORMAL,unreal.MaterialProperty.MP_ROUGHNESS)}
    strength=authored(m,'ScalarParameter','RegionalSurfaceRefinement',parameter_name='RegionalSurfaceRefinement',default_value=1.)
    tile=authored(m,'ScalarParameter','RegionalAccentTileCm',parameter_name='RegionalAccentTileCm',default_value=200.)
    biome=tagged(m,STYLE+'RegionalBiomeSlot')
    roles={'BaseColor':('Diffuse','SAMPLERTYPE_COLOR'),
           'Roughness':('Roughness','SAMPLERTYPE_MASKS'),
           'Normal':('NormalDX','SAMPLERTYPE_NORMAL')}
    for label,(role,sampler) in roles.items():
        expression=tagged(m,STYLE+'Wrap'+label)
        if expression.get_class()!=unreal.MaterialExpressionCustom.static_class():raise RuntimeError('Owned style is not Custom '+label)
        texture=unreal.load_asset('/Game/Environment/Textures/PolyHaven/T_LeafyGrass_'+role)
        if not isinstance(texture,unreal.Texture2D):raise RuntimeError('Existing verified accent PBR texture missing '+role)
        accent=authored(m,'TextureObjectParameter','RegionalAccent'+role,parameter_name='RegionalAccent'+role,
                        texture=texture,sampler_type=getattr(unreal.MaterialSamplerType,sampler))
        for key,value in {'Accent':accent,'AccentTile':tile,'BiomeSlot':biome,'Refinement':strength}.items():custom_input(expression,key,value)
        code=str(expression.get_editor_property('code'))
        if '// AS_GROUND_REFINEMENT_V1:COMMON' not in code:
            code=replace_once(code,'float amount=saturate(Coverage)*saturate(Enabled)*saturate(Ready);',
                              'float amount=saturate(Coverage)*saturate(Enabled)*saturate(Ready);\n'+REFINE)
            if label=='BaseColor':
                code=replace_once(code,'float3 base=Texture2DSample(Base,BaseSampler,qb.xy).rgb;',
                    'float3 base=lerp(Texture2DSample(Base,BaseSampler,qb.xy).rgb,Texture2DSample(Accent,AccentSampler,qa.xy).rgb,accentWeight);')
                code=replace_once(code,'float mix=lerp(.20,.78,smoothstep(25000.,250000.,PixelDepthCm))*inside;',
                    '// Historical imagery is broad colour context; local PBR remains readable below2km.\n float mix=lerp(.06,.42,smoothstep(50000.,240000.,PixelDepthCm))*inside;')
            elif label=='Roughness':
                code=replace_once(code,'float base=Texture2DSample(Base,BaseSampler,qb.xy).r;',
                    'float base=lerp(Texture2DSample(Base,BaseSampler,qb.xy).r,Texture2DSample(Accent,AccentSampler,qa.xy).r,accentWeight);')
            else:
                code=replace_once(code,'float2 b=Texture2DSample(Base,BaseSampler,qb.xy).rg*2.-1.;',
                    'float2 b=lerp(Texture2DSample(Base,BaseSampler,qb.xy).rg,Texture2DSample(Accent,AccentSampler,qa.xy).rg,accentWeight)*2.-1.;')
        code=share_pbr_samplers(code)
        expression.set_editor_property('code',code)
    shared=tagged(m,GLOBAL+'SharedPhotoAndOpticBlend')
    shared_code=str(shared.get_editor_property('code'))
    if '// AS_GROUND_REFINEMENT_V1:PHOTO' not in shared_code:
        shared_code=replace_once(shared_code,'float farFactor=smoothstep(40000.,200000.,eyeDistance);',
             '// AS_GROUND_REFINEMENT_V1:PHOTO\nfloat farFactor=smoothstep(100000.,200000.,eyeDistance);')
        shared.set_editor_property('code',shared_code)
    if any(connection(m,p)!=value for p,value in before.items()):raise RuntimeError('Protected output changed')
    if any(connection(m,p)!=value for p,value in outputs.items()):raise RuntimeError('Output wrapper connection changed')
    # Pixel colour/normal/roughness only. Water fields, photo contents/UV,
    # source masks, curvature and underlying output ownership are untouched.
    return {'material':m.get_path_name(),'extra_unique_pbr_sets':1,'extra_pbr_texture_reads_max':3,
            'shader_changes':'existing owned Custom bodies only; output connections preserved',
            'shared_sampler':'GetMaterialSharedSampler(TextureSampler,Material.Wrap_WorldGroupSettings)',
            'compiled_statistics_pending':True,'geology_or_current_ground_truth_claim':False}

def main():
    materials=[unreal.load_asset(p) for p in PATHS]
    if not all(isinstance(m,unreal.Material) for m in materials):raise RuntimeError('Both original material graphs required')
    rows=[refine(m) for m in materials]
    for m,row in zip(materials,rows):
        errors=list(L.recompile_material(m))
        stats=L.get_statistics(m) # FinishCompilation in native UE5.8 implementation.
        data={name:int(stats.get_editor_property(name)) for name in
              ('num_samplers','num_pixel_shader_instructions','num_vertex_shader_instructions','num_pixel_texture_samples')}
        if errors or data['num_pixel_shader_instructions']<=0 or data['num_vertex_shader_instructions']<=0 or not 1<=data['num_samplers']<=16:
            raise RuntimeError('Unproven/overbudget ground shader; NOTHING saved: '+m.get_path_name()+' '+repr(errors)+' '+repr(data))
        row.update(compiled_statistics_pending=False,shader_statistics=data,compile_errors=[])
    for m in materials:
        if not A.save_loaded_asset(m):raise RuntimeError('Ground refinement save failed '+m.get_path_name())
    report={'valid':True,'materials':rows,'camera_or_simulation_changed':False,
            'existing_source_mask_semantics':'waterR/forestG/agricultureB unchanged',
            'source_reference':'existing verified CC0 textures plus unchanged EOX2016/17 optical context',
            'limitations':'One extra PBR set does not add mapped urban cover or parcel geology. RHI images/performance and Metal remain separate acceptance.'}
    dest=ROOT/'Saved/PresentationValidation/ground_surface_refinement.json';dest.parent.mkdir(parents=True,exist_ok=True)
    dest.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    unreal.log_warning('AFTERSHOCK_GROUND_REFINEMENT_READY '+json.dumps(report))

if __name__=='__main__':main()
