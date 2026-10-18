"""Import ONLY the three verified ambientCG Grass001 PBR textures.

No mesh/material import, no shader rewiring, no source imagery/classification,
no Engine launch. Parent runs this through its coordinated Editor process.
"""
from pathlib import Path
import ast
import hashlib
import json
import runpy
import unreal

ROOT=Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())).resolve()
SOURCE=ROOT/'_Imports/GlobalEnvironment'
MANIFEST=SOURCE/'GreenMeadow/SOURCE_MANIFEST.json'
A=unreal.EditorAssetLibrary


def main():
    data=json.loads(MANIFEST.read_text(encoding='utf-8'))
    if data['asset_id']!='ambientcg_grass001' or data['license']!='CC0-1.0' or len(data['files'])!=3:
        raise RuntimeError('Expected exact verified three-map greenmeadow source')
    if data.get('source_technique')!='surface-fully-procedural':
        raise RuntimeError('Source technique/provenance changed; do not claim a scan')
    cook=(ROOT/'Config/DefaultGame.ini').read_text(encoding='utf-8')
    if '+DirectoriesToAlwaysCook=(Path="/Game/Environment/Textures/Regional")' not in cook:
        raise RuntimeError('Existing regionaltexture cook policy missing; parent must review Config before runtime use')
    helper=ROOT/'scripts/ue_author_global_environment.py'
    tree=ast.parse(helper.read_text(encoding='utf-8'))
    guard=any(isinstance(n,ast.If) and isinstance(n.test,ast.Compare)
              and isinstance(n.test.left,ast.Name) and n.test.left.id=='__name__'
              and any(isinstance(c,ast.Constant) and c.value=='__main__' for c in n.test.comparators)
              for n in tree.body)
    if not guard:raise RuntimeError('Guarded texture helper required; refusing full environment importer')
    # All sources preflight before native asset work. Never invoke helper.main.
    for row in data['files']:
        p=SOURCE/row['relative_path']
        if hashlib.sha256(p.read_bytes()).hexdigest()!=row['sha256']:
            raise RuntimeError('Greenmeadow source changed '+str(p))
        if not row['native_texture_path'].startswith('/Game/Environment/Textures/Regional/T_RegionalGreenMeadow_'):
            raise RuntimeError('Unexpected target package '+row['native_texture_path'])
    defs=runpy.run_path(str(helper),run_name='aftershock_greenmeadow_texture_helpers')
    rows=[]
    for row in data['files']:
        filename=SOURCE/row['relative_path'];dest=row['native_texture_path']
        existed=A.does_asset_exist(dest)
        texture=defs['import_file'](filename,dest)
        if not isinstance(texture,unreal.Texture2D):raise RuntimeError('Expected Texture2D '+dest)
        normal=row['role']=='normal_dx';srgb=row['role']=='diffuse'
        compression=unreal.TextureCompressionSettings.TC_NORMALMAP if normal else unreal.TextureCompressionSettings.TC_DEFAULT if srgb else unreal.TextureCompressionSettings.TC_MASKS
        settings={'srgb':srgb,'compression_settings':compression,'never_stream':False,
                  'max_texture_size':1024,'lod_group':unreal.TextureGroup.TEXTUREGROUP_WORLD_NORMAL_MAP if normal else unreal.TextureGroup.TEXTUREGROUP_WORLD,
                  'address_x':unreal.TextureAddress.TA_WRAP,'address_y':unreal.TextureAddress.TA_WRAP}
        if normal:settings['flip_green_channel']=False
        for key,value in settings.items():texture.set_editor_property(key,value)
        if not A.save_loaded_asset(texture):raise RuntimeError('Native texture save failed '+dest)
        import_data=texture.get_editor_property('asset_import_data')
        native_source=Path(import_data.get_first_filename()) if import_data else None
        if not native_source or not native_source.is_file() or hashlib.sha256(native_source.read_bytes()).hexdigest()!=row['sha256']:
            raise RuntimeError('Native texture source does not match selected grass '+dest)
        if bool(texture.get_editor_property('srgb'))!=srgb or texture.get_editor_property('compression_settings')!=compression:
            raise RuntimeError('Native texture readback mismatch '+dest)
        rows.append({'path':texture.get_path_name(),'role':row['role'],'source_sha256':row['sha256'],
                     'srgb':srgb,'normal_directx_green_flip':False if normal else None,
                     'max_size':1024,'streaming':True,'existing_reused':existed,
                     'native_import_filename_matches_source':True})
    report={'valid':True,'textures':rows,'imported_texture_count':sum(not x['existing_reused'] for x in rows),
            'meshes_imported':0,'materials_imported_or_rewired':0,'new_scalar_parameters':0,
            'runtime_tile_cm':140.,'source_technique':'procedural PBR, not photogrammetry',
            'cook':'Existing regional texture directory cook inclusion verified',
            'acceptance_scope':'Native three texture settings and source-file hashes only; actual runtime binding/colour/RHI quality remains to verify'}
    out=ROOT/'Saved/PresentationValidation/green_meadow_native.json';out.parent.mkdir(parents=True,exist_ok=True)
    out.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    unreal.log_warning('AFTERSHOCK_GREEN_MEADOW_NATIVE '+json.dumps(report))


if __name__=='__main__':main()
