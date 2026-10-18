"""Native owned-art importer. Idempotent, bounded, source hashes verified; no gameplay writes."""
from pathlib import Path
import hashlib,json,math
import unreal
ROOT=Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())).resolve()
SOURCE=ROOT/'_Imports/GlobalEnvironment'
A=unreal.EditorAssetLibrary; L=unreal.MaterialEditingLibrary
TOOLS=unreal.AssetToolsHelpers.get_asset_tools(); SM=unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem) or unreal.get_default_object(unreal.StaticMeshEditorSubsystem)
REPORT={'schema':1,'valid':False,'textures':[],'materials':[],'meshes':[],'source':'Verified CC0 PBR + original project geometry','physics_modified':False}
OUT=ROOT/('Saved/PresentationValidation/global_environment_broadleaf_native.json' if 'AftershockBroadleafOnly' in unreal.SystemLibrary.get_command_line() else 'Saved/PresentationValidation/global_environment_native.json');OUT.parent.mkdir(parents=True,exist_ok=True)
def checkpoint():OUT.write_text(json.dumps(REPORT,indent=2)+'\n',encoding='utf-8')
def save(asset):
    if not A.save_loaded_asset(asset):raise RuntimeError('Save failed '+asset.get_path_name())
def node(m,kind,name='',**props):
    n=L.create_material_expression(m,getattr(unreal,'MaterialExpression'+kind),0,0)
    if name:n.set_editor_property('desc','ASGlobalV1:'+name)
    for k,v in props.items():n.set_editor_property(k,v)
    return n
def link(a,b,pin='',output=''):
    if not L.connect_material_expressions(a,output,b,pin):raise RuntimeError('Link '+str(pin))
def prop(m,n,p,output=''):
    if not L.connect_material_property(n,output,getattr(unreal.MaterialProperty,'MP_'+p)):raise RuntimeError('Output '+p)
def c(m,v):return node(m,'Constant',r=float(v))
def vec(m,v):return node(m,'Constant3Vector',constant=unreal.LinearColor(*v,1))
def sc(m,n,v):return node(m,'ScalarParameter',n,parameter_name=n,default_value=float(v))
def vc(m,n,v):return node(m,'VectorParameter',n,parameter_name=n,default_value=unreal.LinearColor(*v,1))
def op(m,k,a,b):
    n=node(m,k);link(a,n,'A');link(b,n,'B');return n
def mul(m,a,b):return op(m,'Multiply',a,b)
def add(m,a,b):return op(m,'Add',a,b)
def lerp(m,a,b,t):
    n=node(m,'LinearInterpolate');link(a,n,'A');link(b,n,'B');link(t,n,'Alpha');return n
def mask(m,a,r=False,g=False,b=False):
    n=node(m,'ComponentMask',r=r,g=g,b=b,a=False);link(a,n);return n
def inst(m,index,default=0):
    n=node(m,'PerInstanceCustomData',data_index=index,const_default_value=default)
    i=node(m,'VertexInterpolator');link(n,i);return i
def import_file(path,dest,options=None):
    if A.does_asset_exist(dest):return unreal.load_asset(dest)
    task=unreal.AssetImportTask();task.set_editor_property('filename',str(path));task.set_editor_property('destination_path',dest.rsplit('/',1)[0]);task.set_editor_property('destination_name',dest.rsplit('/',1)[1]);task.set_editor_property('automated',True);task.set_editor_property('save',True)
    if options:task.set_editor_property('options',options)
    TOOLS.import_asset_tasks([task]);obj=unreal.load_asset(dest)
    if not obj:raise RuntimeError('Import failed '+dest)
    return obj
def tex(m,name,path,uv,normal=False,linear=False):
    t=unreal.load_asset(path)
    if not t:raise RuntimeError('Missing texture '+path)
    n=node(m,'TextureSampleParameter2D',name,parameter_name=name,texture=t,sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL if normal else unreal.MaterialSamplerType.SAMPLERTYPE_MASKS if linear else unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
    link(uv,n,'UVs');return n
def material(path,kind,texture_set=None,leaf=False):
    m=unreal.load_asset(path) if A.does_asset_exist(path) else TOOLS.create_asset(path.rsplit('/',1)[1],path.rsplit('/',1)[0],unreal.Material,unreal.MaterialFactoryNew())
    if L.get_material_expressions(m):
        if not any(str(x.get_editor_property('desc'))=='ASGlobalV1:Complete' for x in L.get_material_expressions(m)):raise RuntimeError('Preserve unowned material '+path)
        REPORT['materials'].append({'path':path,'reused':True});return m
    sc(m,'GlobalEnvironmentArtVersion',1).set_editor_property('desc','ASGlobalV1:Version')
    m.set_editor_property('two_sided',bool(leaf));m.set_editor_property('shading_model',unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    uv=node(m,'TextureCoordinate',coordinate_index=0)
    raw=vec(m,{'Metal':(.22,.25,.27),'Glass':(.025,.055,.065),'Leaf':(.055,.16,.025),'Bark':(.16,.085,.035),'Concrete':(.42,.41,.38),'Brick':(.37,.14,.08)}.get(kind,(.65,.64,.60)))
    rough=c(m,.68);normal=vec(m,(0,0,1))
    if texture_set:
        raw=tex(m,'SurfaceAlbedo',texture_set['diffuse'],uv)
        normal=tex(m,'SurfaceNormal',texture_set['normal_dx'],uv,normal=True)
        if 'roughness' in texture_set:rough=mask(m,tex(m,'SurfaceRoughness',texture_set['roughness'],uv,linear=True),r=True)
        normal=lerp(m,vec(m,(0,0,1)),normal,sc(m,'SurfaceNormalStrength',.65))
    if kind in ('Leaf','Bark'):
        vertex=node(m,'VertexColor');variation=add(m,c(m,.72),mul(m,mask(m,vertex,b=True),c(m,.4)))
        raw=mul(m,raw,variation)
        if leaf:prop(m,mul(m,raw,c(m,.35)),'SUBSURFACE_COLOR');m.set_editor_property('shading_model',unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)
        time=node(m,'Time');pos=node(m,'WorldPosition');phase=add(m,mul(m,mask(m,pos,r=True),c(m,.006)),mul(m,mask(m,pos,g=True),c(m,.004)))
        sine=node(m,'Sine');link(add(m,phase,mul(m,time,c(m,.7))),sine)
        weight=mask(m,vertex,r=True);local=node(m,'PerInstanceCustomData',data_index=0,const_default_value=0)
        amount=mul(m,mul(m,mul(m,sine,weight),local),mul(m,sc(m,'StormStrength',0),sc(m,'WindBendCm',35 if leaf else 12)))
        direction=vc(m,'StormDirection',(1,0,0));prop(m,mul(m,direction,amount),'WORLD_POSITION_OFFSET')
    else:
        damage=inst(m,0,0);tone=inst(m,4,1)
        if kind not in ('Glass','Roof','Metal'):
            tint=node(m,'PerInstanceCustomData3Vector',data_index=8,const_default_value=unreal.LinearColor(1,1,1,1));pixel=node(m,'VertexInterpolator');link(tint,pixel);raw=mul(m,mul(m,raw,pixel),tone)
        # All four canonical channels remain visual inputs; they never change simulation.
        retrofit=inst(m,1,0);operation=inst(m,3,1)
        raw=mul(m,raw,lerp(m,c(m,1),c(m,.42),damage))
        if kind!='Glass':
            position=node(m,'WorldPosition');x=mask(m,position,r=True);z=mask(m,position,b=True)
            fracture=node(m,'Sine');link(add(m,mul(m,x,c(m,.037)),mul(m,z,c(m,.051))),fracture)
            absolute=node(m,'Abs');link(fracture,absolute)
            crack=node(m,'SmoothStep');link(absolute,crack,'Value');link(c(m,.018),crack,'Min');link(c(m,.055),crack,'Max')
            inverse=node(m,'OneMinus');link(crack,inverse)
            cracks=mul(m,inverse,damage);raw=lerp(m,raw,mul(m,raw,c(m,.22)),cracks)
            # A subtle reinforced/service colour signal uses existing saved state.
            raw=lerp(m,raw,mul(m,raw,vec(m,(.90,1.015,.95))),mul(m,retrofit,c(m,.65)))
            raw=mul(m,raw,lerp(m,c(m,.76),c(m,1),operation))
        if kind=='Glass':
            rough=c(m,.16);power=inst(m,2,1);prop(m,mul(m,mul(m,vec(m,(1,.65,.28)),power),sc(m,'WindowEmission',.08)),'EMISSIVE_COLOR')
        if kind=='Metal':rough=c(m,.36);prop(m,c(m,.72),'METALLIC')
    prop(m,raw,'BASE_COLOR');prop(m,rough,'ROUGHNESS');prop(m,normal,'NORMAL')
    L.set_base_material_usage(m,unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES,True)
    L.layout_material_expressions(m);errors=L.recompile_material(m)
    if errors:raise RuntimeError('Material compile '+path+str(list(errors)))
    node(m,'Constant','Complete',r=1);save(m);REPORT['materials'].append({'path':path,'kind':kind,'compiled':True,'instanced':True,'owned_graph':True});checkpoint();return m

def main():
    source=json.loads((SOURCE/'SOURCE_MANIFEST.json').read_text(encoding='utf-8'));textures={}
    for row in source['files']:
        path=SOURCE/row['relative_path']
        if hashlib.sha256(path.read_bytes()).hexdigest()!=row['sha256']:raise RuntimeError('Source changed '+str(path))
        suffix={'diffuse':'C','normal_dx':'N','roughness':'R'}[row['role']]
        dest=row.get('native_texture_path','/Game/Environment/Textures/GlobalPBR/T_'+row['asset_id']+'_'+suffix)
        t=import_file(path,dest);normal=row['role']=='normal_dx';srgb=row['role']=='diffuse'
        t.set_editor_property('srgb',srgb);t.set_editor_property('never_stream',False);t.set_editor_property('max_texture_size',row['dimensions_px'][0]);t.set_editor_property('compression_settings',unreal.TextureCompressionSettings.TC_NORMALMAP if normal else unreal.TextureCompressionSettings.TC_DEFAULT if srgb else unreal.TextureCompressionSettings.TC_MASKS)
        t.set_editor_property('lod_group',unreal.TextureGroup.TEXTUREGROUP_WORLD_NORMAL_MAP if normal else unreal.TextureGroup.TEXTUREGROUP_WORLD)
        save(t);textures.setdefault(row['asset_id'],{})[row['role']]=dest;REPORT['textures'].append({'path':dest,'source_sha256':row['sha256'],'streaming':True,'srgb':srgb});checkpoint()
    mats={}
    for kind,sourceid in [('Plaster','painted_plaster_wall'),('Wood','wood_planks'),('Roof','clay_roof_tiles_02'),('Concrete',None),('Brick',None),('Metal',None),('Glass',None)]:
        texture_set=textures.get(sourceid)
        if kind in ('Brick','Concrete'):
            texture_set={'diffuse':'/Game/Aftershock/CitySample/Textures/T_'+kind+'_C','normal_dx':'/Game/Aftershock/CitySample/Textures/T_'+kind+'_N'}
        mats[kind]=material('/Game/Buildings/Architecture/Materials/M_Arch_'+kind,kind,texture_set)
    bark=material('/Game/Environment/Materials/Realistic/M_EnvironmentBark','Bark',textures['wood_planks'])
    leaves={family:material('/Game/Environment/Materials/Realistic/M_Environment'+family,'Leaf',leaf=True) for family in ('Grass','Shrub','Conifer','Palm','Broadleaf')}
    options=unreal.FbxImportUI()
    for k,v in [('import_mesh',True),('import_materials',False),('import_textures',False),('automated_import_should_detect_type',False),('mesh_type_to_import',unreal.FBXImportType.FBXIT_STATIC_MESH)]:options.set_editor_property(k,v)
    data=unreal.FbxStaticMeshImportData()
    for k,v in [('combine_meshes',True),('auto_generate_collision',False),('generate_lightmap_u_vs',False),('normal_import_method',unreal.FBXNormalImportMethod.FBXNIM_IMPORT_NORMALS),('vertex_color_import_option',unreal.VertexColorImportOption.REPLACE)]:data.set_editor_property(k,v)
    options.set_editor_property('static_mesh_import_data',data)
    manifests=[('ARCHITECTURE_GEOMETRY_MANIFEST.json','architecture_geometry',False),('ENVIRONMENT_GEOMETRY_MANIFEST.json','environment_geometry',True),('BROADLEAF_GEOMETRY_MANIFEST.json','broadleaf_geometry',True)]
    if 'AftershockBroadleafOnly' in unreal.SystemLibrary.get_command_line():manifests=manifests[-1:]
    for filename,folder,environment in manifests:
        manifest=json.loads((SOURCE/filename).read_text(encoding='utf-8'))
        for row in manifest['assets']:
            src=row['lods'][0] if environment else row
            path=SOURCE/folder/src['source_file']
            if hashlib.sha256(path.read_bytes()).hexdigest()!=src['sha256']:raise RuntimeError('Mesh source hash '+str(path))
            mesh=import_file(path,row['target_package'],options)
            if not isinstance(mesh,unreal.StaticMesh):raise RuntimeError('Not mesh '+row['target_package'])
            box=mesh.get_bounding_box();expected=src['bounds_cm']['size'];actual=[box.max.x-box.min.x,box.max.y-box.min.y,box.max.z-box.min.z]
            if any(abs(a-b)>max(.5,b*.001) for a,b in zip(actual,expected)):raise RuntimeError('Native cm bbox mismatch '+row['asset_name']+str(actual)+str(expected))
            if environment:
                for lod in (1,2):
                    lodsrc=SOURCE/folder/row['lods'][lod]['source_file']
                    if hashlib.sha256(lodsrc.read_bytes()).hexdigest()!=row['lods'][lod]['sha256']:raise RuntimeError('LOD source hash '+str(lodsrc))
                    if SM.import_lod(mesh,lod,str(lodsrc))!=lod:raise RuntimeError('Explicit LOD '+str(lodsrc))
                family='Grass' if row['family']=='GrassClump' else row['family'];mesh.set_material(0,bark);mesh.set_material(1,leaves[family]);SM.set_lod_screen_sizes(mesh,[1.,.15,.045])
            else:
                reduction=unreal.StaticMeshReductionOptions();reduction.set_editor_property('auto_compute_lod_screen_size',False)
                settings=[]
                for percent,screen in [(1.,1.),(.40,.16),(.12,.045)]:
                    s=unreal.StaticMeshReductionSettings();s.set_editor_property('percent_triangles',percent);s.set_editor_property('screen_size',screen);settings.append(s)
                reduction.set_editor_property('reduction_settings',settings)
                if SM.set_lods(mesh,reduction)<3:raise RuntimeError('Architecture real LOD creation '+row['asset_name'])
                roof='Roof' in row['asset_name']
                for slot,entry in enumerate(mesh.get_editor_property('static_materials')):
                    role=str(entry.get_editor_property('material_slot_name'))
                    kind='Metal' if 'Metal' in role else 'Glass' if 'Glass' in role else 'Roof' if roof and 'Roof' in role else 'Plaster'
                    mesh.set_material(slot,mats[kind])
            ns=mesh.get_editor_property('nanite_settings');ns.set_editor_property('enabled',False);mesh.set_editor_property('nanite_settings',ns)
            for lod in range(SM.get_lod_count(mesh)):
                build=SM.get_lod_build_settings(mesh,lod);build.set_editor_property('generate_lightmap_u_vs',False);build.set_editor_property('recompute_tangents',True);SM.set_lod_build_settings(mesh,lod,build)
            save(mesh);REPORT['meshes'].append({'path':row['target_package'],'bounds_cm':actual,'lod_count':SM.get_lod_count(mesh),'source_sha256':src['sha256'],'material_slots':len(mesh.get_editor_property('static_materials'))});checkpoint()
            unreal.log_warning('AFTERSHOCK_GLOBAL_ART_IMPORTED '+row['asset_name'])
    ghost=unreal.load_asset('/Game/Buildings/Materials/M_BuildingGhost')
    if not isinstance(ghost,unreal.Material):raise RuntimeError('Ghost parent missing')
    L.set_base_material_usage(ghost,unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES,True)
    errors=L.recompile_material(ghost)
    if errors:raise RuntimeError('Ghost HISM shader '+str(errors))
    save(ghost);REPORT['ghost_hism_usage']=True
    REPORT['valid']=True;checkpoint()
if __name__=='__main__':
    try:main()
    except Exception as exc:REPORT['error']=repr(exc);checkpoint();raise
