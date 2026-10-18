"""Native microvariation refinement after real R6 Kalkanli art review.

No new texture, sample, sampler, simulation input, geometry or classification.
Reduces coherent tiled-normal bands; does not invent crop rows/geology/grass data.
"""
from pathlib import Path
import json
import unreal

L=unreal.MaterialEditingLibrary
A=unreal.EditorAssetLibrary
ROOT=Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())).resolve()
TAG='AFTERSHOCK_REGIONAL_STYLE_V1:'
MARKER='// AS_GROUND_MICROVARIATION_V2'
MATERIALS=('/Game/Environment/Materials/Presentation/M_PresentationGroundPBR',
           '/Game/Environment/Materials/Presentation/M_AftershockRegionalBlend')
PROPERTIES=(unreal.MaterialProperty.MP_BASE_COLOR,unreal.MaterialProperty.MP_NORMAL,
            unreal.MaterialProperty.MP_ROUGHNESS,unreal.MaterialProperty.MP_EMISSIVE_COLOR,
            unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET,unreal.MaterialProperty.MP_OPACITY,
            unreal.MaterialProperty.MP_OPACITY_MASK,unreal.MaterialProperty.MP_METALLIC)

def connection(material,prop):
    return (L.get_material_property_input_node(material,prop),
            str(L.get_material_property_input_node_output_name(material,prop)))

def tagged(material,label):
    matches=[e for e in L.get_material_expressions(material) if str(e.get_editor_property('desc'))==TAG+label]
    if len(matches)!=1:raise RuntimeError('Expected exact existing owned node '+label)
    return matches[0]

def replace_once(code,old,new):
    if code.count(old)!=1:raise RuntimeError('Owned V1 shader anchor changed '+old[:100])
    return code.replace(old,new,1)

def refine(material):
    before={p:connection(material,p) for p in PROPERTIES}
    textures_before={e.get_path_name():str(e.get_editor_property('texture').get_path_name())
                     for e in L.get_material_expressions(material)
                     if e.get_class().get_name() in ('MaterialExpressionTextureObjectParameter','MaterialExpressionTextureSampleParameter2D')
                     and e.get_editor_property('texture') is not None}
    rows=[]
    for label in ('BaseColor','Normal','Roughness'):
        expression=tagged(material,'Wrap'+label)
        if expression.get_class()!=unreal.MaterialExpressionCustom.static_class():raise RuntimeError('Owned node no longer Custom '+label)
        code=str(expression.get_editor_property('code'))
        if MARKER not in code:
            if '// AS_GROUND_REFINEMENT_V1:COMMON' not in code:raise RuntimeError('Run accepted V1 refinement before V2')
            for uv,amplitude in (('qb','1.35'),('qr','1.65'),('qa','1.35')):
                old=f'{uv}+=(float3(domain.noise({uv}.xy*.23+11.7),domain.noise({uv}.yz*.19+37.3),domain.noise({uv}.zx*.27+71.9))-.5)*{amplitude};'
                # Flat terrain previously made Y phase depend only on world Y.
                # Couple BOTH XY phases continuously; no square tile reset.
                new=f'''float2 {uv}Domain=float2({uv}.x+{uv}.z*.37,{uv}.y-{uv}.z*.29);
{uv}+=(float3(domain.noise({uv}Domain*.23+float2(11.7,29.3)),domain.noise(float2({uv}Domain.x*.83-{uv}Domain.y*.56,{uv}Domain.x*.56+{uv}Domain.y*.83)*.21+float2(37.3,9.7)),domain.noise({uv}.zx*.27+71.9))-.5)*{amplitude};'''
                code=replace_once(code,old,new)
            code=replace_once(code,'// AS_GROUND_REFINEMENT_V1:COMMON',MARKER+'''
// Same meso-scale organic patch in all three PBR roles. Artistic surface
// composition only; mapped forest/farmland membership remains unchanged.
float surfaceMeso=domain.noise(P.xy/1700.+float2(73.9,19.7));
// AS_GROUND_REFINEMENT_V1:COMMON''')
            code=replace_once(code,'(.12+.6*saturate(Wetness))*(1.-agriculture)*(.35+.65*forest)',
                              '(.32+.55*saturate(Wetness))*(1.-agriculture*.55)*(.75+.25*forest)')
            code=replace_once(code,'lerp(.3,1.,smoothstep(.28,.72,macroVariation))',
                              'lerp(.18,1.,smoothstep(.25,.70,surfaceMeso))')
            if label=='BaseColor':
                # OSM farmland is not evidence of freshly ploughed bare soil.
                # Preserve the source mask but avoid flattening nearly half of
                # the real scan into one uniform brown swatch.
                code=replace_once(code,'regional=lerp(regional,lerp(DryTint*.47,DryTint*.82,macroVariation),agriculture*.48);',
                                  'regional=lerp(regional,regional*lerp(float3(.96,.94,.87),float3(1.03,1.01,.94),surfaceMeso),agriculture*.18);')
            if label=='Normal':
                code=replace_once(code,'float3 baseWorld=normalize(ng+float3(b*.38,0.));',
                                  'float3 baseWorld=normalize(ng+float3(b*.22,0.));')
            expression.set_editor_property('code',code)
        rows.append({'label':label,'v2_marker_present':MARKER in code})
    if {p:connection(material,p) for p in PROPERTIES}!=before:raise RuntimeError('A material output connection changed')
    textures_after={e.get_path_name():str(e.get_editor_property('texture').get_path_name())
                    for e in L.get_material_expressions(material)
                    if e.get_class().get_name() in ('MaterialExpressionTextureObjectParameter','MaterialExpressionTextureSampleParameter2D')
                    and e.get_editor_property('texture') is not None}
    if textures_after!=textures_before:raise RuntimeError('A texture/source binding changed')
    return {'material':material.get_path_name(),'owned_custom_nodes':rows,'output_connections_unchanged':True,
            'texture_bindings_unchanged':True,'new_texture_samples':0,'new_samplers':0}

def main():
    materials=[unreal.load_asset(path) for path in MATERIALS]
    if not all(isinstance(m,unreal.Material) for m in materials):raise RuntimeError('Existing two materials required')
    rows=[refine(m) for m in materials]
    for m,row in zip(materials,rows):
        errors=list(L.recompile_material(m));stats=L.get_statistics(m)
        data={key:int(stats.get_editor_property(key)) for key in
              ('num_samplers','num_pixel_shader_instructions','num_vertex_shader_instructions','num_pixel_texture_samples')}
        if errors or not 1<=data['num_samplers']<=16 or data['num_pixel_shader_instructions']<=0 or data['num_vertex_shader_instructions']<=0:
            raise RuntimeError('V2 compile/resource budget failed; nothing saved '+m.get_path_name()+' '+repr(errors)+' '+repr(data))
        row['statistics']=data;row['compile_errors']=[]
    for m in materials:
        if not A.save_loaded_asset(m):raise RuntimeError('Could not save V2 material '+m.get_path_name())
    report={'valid':True,'materials':rows,'source_scope':'Existing CC0 PBR maps and existing mapped forest/agriculture only',
            'wpo_geometry_water_classification_changed':False,'physics_or_saved_state_changed':False,
            'photographic_far_context_changed':False,'scope':'Native graph/compiled resource only. New real RHI visual comparison and profiles remain required.'}
    output=ROOT/'Saved/PresentationValidation/ground_surface_microvariation_v2.json';output.parent.mkdir(parents=True,exist_ok=True)
    output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    unreal.log_warning('AFTERSHOCK_GROUND_MICROVARIATION_V2 '+json.dumps(report))

if __name__=='__main__':main()
