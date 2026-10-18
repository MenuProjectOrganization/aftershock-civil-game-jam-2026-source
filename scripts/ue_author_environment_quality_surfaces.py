"""Add bounded graphics controls while preserving canonical WPO/opacity/arrival outputs."""
from pathlib import Path
import json
import unreal
L=unreal.MaterialEditingLibrary; A=unreal.EditorAssetLibrary
TAG='AS_ENVIRONMENT_QUALITY_V1:'
REPORT={'valid':False,'materials':[],'physics_changed':False}
ROOT=Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())).resolve()
def node(m,kind,label,**props):
    matches=[n for n in L.get_material_expressions(m) if str(n.get_editor_property('desc'))==TAG+label]
    if len(matches)>1:raise RuntimeError('Duplicate quality node '+label)
    n=matches[0] if matches else L.create_material_expression(m,getattr(unreal,'MaterialExpression'+kind))
    n.set_editor_property('desc',TAG+label)
    for k,v in props.items():n.set_editor_property(k,v)
    return n
def link(a,b,p='',o=''):
    if not L.connect_material_expressions(a,o,b,p):raise RuntimeError('Quality link '+p)
def connection(m,p):return (L.get_material_property_input_node(m,p),str(L.get_material_property_input_node_output_name(m,p)))
def wrap_normal(m,water):
    name='GlobalWaterNormalQuality' if water else 'GlobalGroundNormalQuality'
    q=node(m,'ScalarParameter',name,parameter_name=name,default_value=1.)
    original,out=connection(m,unreal.MaterialProperty.MP_NORMAL)
    if not original:raise RuntimeError('Original surface normal missing '+m.get_path_name())
    # Preserve canonical direct-output nodes, their labels, source physics and all
    # existing UV/arrival/data connections. Only final shading normal is blended.
    if original.get_class().get_name()=='MaterialExpressionCustom':
        code=str(original.get_editor_property('code'))
        if 'AS_ENVIRONMENT_NORMAL_QUALITY' not in code:
            before,expression=code.rsplit('return ',1);expression=expression.strip()
            if not expression.endswith(';'):raise RuntimeError('Unknown normal return shape')
            original.set_editor_property('code',before+'// AS_ENVIRONMENT_NORMAL_QUALITY\nreturn normalize(lerp(float3(0,0,1),('+expression[:-1]+'),GlobalNormalDetail));\n')
            inputs=list(original.get_editor_property('inputs'));entry=unreal.CustomInput();entry.set_editor_property('input_name','GlobalNormalDetail');inputs.append(entry);original.set_editor_property('inputs',inputs)
        link(q,original,'GlobalNormalDetail')
    elif not str(original.get_editor_property('desc')).startswith(TAG):
        flat=node(m,'Constant3Vector','FlatNormal',constant=unreal.LinearColor(0,0,1,1))
        blend=node(m,'LinearInterpolate','NormalQualityBlend');link(flat,blend,'A');link(original,blend,'B',out);link(q,blend,'Alpha')
        n=node(m,'Normalize','NormalQualityOutput');link(blend,n)
        if not L.connect_material_property(n,'',unreal.MaterialProperty.MP_NORMAL):raise RuntimeError('Normal attachment')
    # Foam is visual only. Never scale wetness, physical columns, WPO or alpha.
    if water:
        foam=node(m,'ScalarParameter','GlobalWaterFoamQuality',parameter_name='GlobalWaterFoamQuality',default_value=1.)
        for e in L.get_material_expressions(m):
            if e.get_class().get_name()!='MaterialExpressionCustom':continue
            code=str(e.get_editor_property('code'))
            if 'float foam =' not in code and 'float foam=' not in code:continue
            if 'AS_ENVIRONMENT_FOAM_QUALITY' not in code:
                marker='float3 colour='
                if marker not in code:marker='float3 colour ='
                if marker not in code:continue
                code=code.replace(marker,'// AS_ENVIRONMENT_FOAM_QUALITY\nfoam*=GlobalFoamDetail;\n'+marker,1);e.set_editor_property('code',code)
                inputs=list(e.get_editor_property('inputs'));entry=unreal.CustomInput();entry.set_editor_property('input_name','GlobalFoamDetail');inputs.append(entry);e.set_editor_property('inputs',inputs)
            link(foam,e,'GlobalFoamDetail')
for path,water in [('/Game/Environment/Materials/Presentation/M_PresentationGroundPBR',False),('/Game/Environment/Materials/Presentation/M_AftershockRegionalBlend',False),('/Game/Environment/Materials/Presentation/M_PresentationWater',True),('/Game/Environment/Materials/Presentation/M_CoastalSea',True),('/Game/FX/Aftershock/Materials/M_DisasterFloodVolume',True),('/Game/FX/Aftershock/Materials/M_DisasterTsunamiWave',True)]:
    m=unreal.load_asset(path)
    if not isinstance(m,unreal.Material):raise RuntimeError('Quality surface missing '+path)
    protected={p:connection(m,p) for p in (unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET,unreal.MaterialProperty.MP_OPACITY)}
    wrap_normal(m,water)
    if any(connection(m,p)!=v for p,v in protected.items()):raise RuntimeError('Canonical displacement/coverage changed')
    # Raster SSR on baseline river/sea only; physical disaster material flags stay intact.
    if path.endswith(('M_PresentationWater','M_CoastalSea')):
        m.set_editor_property('translucency_lighting_mode',unreal.TranslucencyLightingMode.TLM_SURFACE_PER_PIXEL_LIGHTING);m.set_editor_property('screen_space_reflections',True)
    errors=L.recompile_material(m)
    if errors:raise RuntimeError('Quality surface compile '+path+str(list(errors)))
    if not A.save_loaded_asset(m):raise RuntimeError('Quality surface save '+path)
    REPORT['materials'].append({'path':path,'compiled':True,'protected_wpo_opacity_connections_unchanged':True,'normal_quality':True,'water':water})
REPORT['valid']=True
out=ROOT/'Saved/PresentationValidation/environment_quality_surfaces.json';out.parent.mkdir(parents=True,exist_ok=True);out.write_text(json.dumps(REPORT,indent=2)+'\n',encoding='utf-8')
