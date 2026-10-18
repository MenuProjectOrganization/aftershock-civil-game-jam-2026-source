"""Original low-poly city services, icons and damage/power material authoring.

Standalone Python generates inspectable OBJ/PNG sources. UE5.8 Python also imports
them and validates material compilation. No downloads or paid/third-party art.
Buildings remain one static mesh and one material each, drawn by the existing ISM.
"""
import hashlib
import json
import math
from pathlib import Path
import struct
import zlib

try:
    import unreal
except ImportError:
    unreal = None

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "_Imports" / "Original" / "CityServices"
NAMES = ("WindTurbine", "Shelter", "Farm", "LivestockFarm", "Generator", "Bakery",
         "Restaurant", "TaxOffice", "Courthouse", "PoliceStation", "BusTerminal",
         "BusStop", "WaterPlant", "Warehouse", "Substation")
PALETTE = ((210,198,171),(111,125,130),(151,64,45),(56,100,55),(28,66,83),
           (45,93,139),(224,179,79),(148,151,148),(91,65,43),(131,96,64),
           (225,224,213),(34,39,41),(64,125,145),(100,135,56),(169,88,56),(86,90,94))


def png(path, width, height, pixels, channels=4):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    rows = b"".join(b"\0" + bytes(pixels[y*width*channels:(y+1)*width*channels]) for y in range(height))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB",width,height,8,6 if channels==4 else 2,0,0,0)) + chunk(b"IDAT",zlib.compress(rows,9)) + chunk(b"IEND",b""))


class Mesh:
    def __init__(self):
        self.vertices, self.faces = [], []

    def polyhedron(self, vertices, faces, color):
        start = len(self.vertices)
        self.vertices.extend(vertices)
        self.faces.extend((tuple(start+i for i in face),color) for face in faces)

    def box(self,x,y,z,w,d,h,color=0,angle=0,tilt=0):
        vertices=[]
        for px,py,pz in ((-1,-1,-1),(1,-1,-1),(1,1,-1),(-1,1,-1),(-1,-1,1),(1,-1,1),(1,1,1),(-1,1,1)):
            px,py,pz=px*w/2,py*d/2,pz*h/2
            py,pz=py*math.cos(tilt)-pz*math.sin(tilt),py*math.sin(tilt)+pz*math.cos(tilt)
            px,py=px*math.cos(angle)-py*math.sin(angle),px*math.sin(angle)+py*math.cos(angle)
            vertices.append((x+px,y+py,z+pz))
        self.polyhedron(vertices,((0,3,2,1),(4,5,6,7),(0,1,5,4),(1,2,6,5),(2,3,7,6),(3,0,4,7)),color)

    def cylinder(self,x,y,z,r,h,color=1,sides=12):
        vertices=[(x+r*math.cos(i*2*math.pi/sides),y+r*math.sin(i*2*math.pi/sides),z+level*h) for level in (0,1) for i in range(sides)]
        faces=[tuple(reversed(range(sides))),tuple(range(sides,sides*2))]
        faces.extend((i,(i+1)%sides,(i+1)%sides+sides,i+sides) for i in range(sides))
        self.polyhedron(vertices,faces,color)

    def roof(self,x,y,z,w,d,rise,color=2):
        self.polyhedron(((x-w/2,y-d/2,z),(x+w/2,y-d/2,z),(x,y-d/2,z+rise),
                         (x-w/2,y+d/2,z),(x+w/2,y+d/2,z),(x,y+d/2,z+rise)),
                        ((0,2,1),(3,4,5),(0,1,4,3),(0,3,5,2),(2,5,4,1)),color)

    def building(self,w=320,d=260,h=260,wall=0,roof=2):
        self.box(0,0,h/2,w,d,h,wall)
        self.roof(0,0,h,w+20,d+20,80,roof)
        for x in (-w*.32,0,w*.32):
            for z in (h*.34,h*.7):
                self.box(x,-d/2-2,z,42,5,53,4)
                self.box(x,d/2+2,z,42,5,53,4)
        self.box(0,-d/2-4,47,48,8,94,9)

    def fence(self):
        for y in (-205,205):
            self.box(0,y,52,430,9,8,9); self.box(0,y,84,430,9,8,9)
        for x in range(-200,201,80):
            for y in (-205,205): self.box(x,y,48,9,9,96,9)
        for x in (-205,205):
            self.box(x,0,52,9,420,8,9); self.box(x,0,84,9,420,8,9)

    def save(self,path):
        lines=["# AFTERSHOCK original authored city service. Units cm. Z up.","o "+path.stem]
        lines += ["v %.4f %.4f %.4f"%v for v in self.vertices]
        uv_count=0; face_lines=[]
        for face,color in self.faces:
            if len(face)==4: local=((0,0),(1,0),(1,1),(0,1))
            else: local=[(.5+.48*math.cos(i*2*math.pi/len(face)),.5+.48*math.sin(i*2*math.pi/len(face))) for i in range(len(face))]
            # Nondegenerate face UVs stay safely inside one palette swatch.
            # This permits valid tangent reconstruction rather than zero-UV bases.
            for u,v in local: lines.append("vt %.6f %.6f"%((color+.16+.68*u)/16,.12+.76*v))
            face_lines.append("f "+" ".join("%d/%d"%(vertex+1,uv_count+i+1) for i,vertex in enumerate(face)))
            uv_count+=len(face)
        lines+=face_lines
        path.write_text("\n".join(lines)+"\n",encoding="ascii")


def make_mesh(name):
    m=Mesh(); m.box(0,0,3,460,460,6,7)
    if name=="Farm":
        m.box(0,0,8,430,430,10,8)
        for x in range(-175,176,50): m.box(x,15,17,22,360,14,13)
        m.box(-145,155,63,110,100,110,9); m.roof(-145,155,118,120,110,40,2)
    elif name=="LivestockFarm":
        m.fence(); m.box(85,90,90,190,170,180,10); m.roof(85,90,180,210,190,75,2)
        for x,y in ((-120,-90),(-40,-120),(-110,30)):
            m.box(x,y,47,65,30,35,10); m.box(x-30,y,63,25,26,26,0)
            for lx in (-21,21): m.box(x+lx,y-10,24,7,7,35,11); m.box(x+lx,y+10,24,7,7,35,11)
    elif name=="Generator":
        m.box(0,0,80,260,170,150,6); m.box(0,0,167,270,180,20,1)
        for x in range(-105,106,30): m.box(x,-87,80,7,5,85,11)
        m.cylinder(85,40,170,14,100,11); m.box(0,88,80,130,5,70,5)
    elif name in ("Bakery","Restaurant"):
        m.building(300,260,215,0,14 if name=="Bakery" else 3)
        m.box(0,-170,150,320,90,12,6 if name=="Bakery" else 3)
        for x in (-130,130): m.box(x,-205,75,8,8,150,10)
        m.box(0,-135,195,230,8,38,6 if name=="Bakery" else 3)
        if name=="Bakery": m.cylinder(85,65,260,20,130,14); m.cylinder(-65,-142,198,16,7,0)
        else:
            for x in (-80,80):
                m.box(x,-180,55,50,48,8,9); m.box(x,-180,28,8,8,50,1)
                m.box(x-36,-180,33,22,22,8,10); m.box(x+36,-180,33,22,22,8,10)
    elif name in ("TaxOffice","Courthouse","PoliceStation"):
        m.building(350,290,360,0 if name!="PoliceStation" else 10,7)
        m.box(0,-170,35,300,70,50,7)
        if name=="Courthouse":
            for x in (-125,-65,65,125): m.cylinder(x,-165,60,15,260,10)
            m.box(0,-170,327,330,90,24,10); m.roof(0,-170,339,330,90,80,10)
        else:
            m.box(0,-150,165,240,22,40,5 if name=="PoliceStation" else 6)
            m.cylinder(160,-175,0,4,415,1); m.box(188,-175,388,52,4,34,5 if name=="PoliceStation" else 2)
    elif name=="BusTerminal":
        m.box(0,50,15,430,260,24,7)
        for x in (-170,0,170): m.box(x,50,130,12,12,230,1)
        m.box(0,50,250,450,280,18,5)
        for x in (-135,30,170): m.box(x,100,56,70,32,8,9)
        m.box(0,-160,8,450,100,8,11)
        for x in range(-175,176,70): m.box(x,-160,14,35,7,4,10)
        m.box(180,50,90,80,150,175,0); m.box(180,-27,110,65,4,60,4)
    elif name=="BusStop":
        for x in (-110,110): m.box(x,35,115,8,8,225,1)
        m.box(0,40,230,280,140,12,5); m.box(0,94,130,250,5,160,4)
        m.box(0,45,52,200,35,10,9)
        for x in (-70,70): m.box(x,45,28,8,25,45,1)
        m.box(155,5,135,6,6,260,1); m.box(155,5,240,45,8,55,5)
    elif name=="WaterPlant":
        for x,y in ((-105,80),(105,80),(-105,-110)):
            m.cylinder(x,y,10,90,55,7,16); m.cylinder(x,y,66,76,3,12,16)
            m.box(x,y,77,150,8,8,1)
        m.box(115,-105,95,125,160,175,0); m.roof(115,-105,182,145,175,35,5)
    elif name=="Warehouse":
        m.building(400,360,250,1,7); m.box(0,-183,97,140,7,190,11)
        for z in range(25,176,30): m.box(0,-187,z,135,4,3,7)
        for x in (-150,150): m.box(x,-208,23,60,55,40,9)
    elif name=="Substation":
        m.fence()
        for x in (-130,0,130):
            m.box(x,30,90,80,130,165,1)
            for y in (-15,55): m.cylinder(x,y,174,11,70,0)
            m.box(x,-38,100,70,5,60,6)
        m.box(0,110,260,360,8,8,1)
        for x in (-165,165): m.box(x,110,135,8,8,250,1)
    elif name=="WindTurbine":
        m.cylinder(0,0,0,40,650,10,12); m.box(0,0,670,80,120,70,1)
        for angle in (0,2*math.pi/3,4*math.pi/3):
            m.box(-50,-130*math.sin(angle),670+130*math.cos(angle),12,24,260,10,tilt=angle)
    else: # Shelter
        m.building(380,330,170,7,3); m.box(0,-170,82,90,10,155,5)
        m.box(0,-178,150,80,9,14,10); m.box(0,-178,150,14,9,70,10)
    return m


def icon(mesh,path):
    # Orthographic software raster of the exact original mesh; no external artwork.
    size=128; pixels=bytearray(size*size*4); depth=[-1e20]*(size*size)
    projected=[((x+y)*.7071,(x-y)*.4082-z*.8165,(x-y)*.5774+z*.5774) for x,y,z in mesh.vertices]
    xmin=min(p[0] for p in projected); xmax=max(p[0] for p in projected); ymin=min(p[1] for p in projected); ymax=max(p[1] for p in projected)
    scale=110/max(xmax-xmin,ymax-ymin,1)
    verts=[((x-(xmin+xmax)/2)*scale+64,(y-(ymin+ymax)/2)*scale+64,z) for x,y,z in projected]
    for face,color in mesh.faces:
        for i in range(1,len(face)-1):
            a,b,c=(verts[j] for j in (face[0],face[i],face[i+1]))
            area=(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0])
            if abs(area)<1e-6: continue
            for y in range(max(0,int(min(a[1],b[1],c[1]))),min(size,int(max(a[1],b[1],c[1]))+1)):
                for x in range(max(0,int(min(a[0],b[0],c[0]))),min(size,int(max(a[0],b[0],c[0]))+1)):
                    u=((b[0]-x)*(c[1]-y)-(b[1]-y)*(c[0]-x))/area
                    v=((c[0]-x)*(a[1]-y)-(c[1]-y)*(a[0]-x))/area; w=1-u-v
                    if min(u,v,w)<0: continue
                    z=u*a[2]+v*b[2]+w*c[2]; index=y*size+x
                    if z<depth[index]: continue
                    depth[index]=z
                    shade=.82 if area>0 else 1.0
                    pixels[index*4:index*4+4]=bytes([int(channel*shade) for channel in PALETTE[color]]+[255])
    png(path,size,size,pixels)


def generate_sources():
    SOURCE.mkdir(parents=True,exist_ok=True)
    pixels=bytearray()
    for y in range(16):
        for x in range(256): pixels.extend(PALETTE[x//16])
    png(SOURCE/"T_ServicePalette.png",256,16,pixels,3)
    report={"provenance":"Original project-authored procedural geometry and orthographic icons", "units":"centimetres", "models":[]}
    for name in NAMES:
        mesh=make_mesh(name); path=SOURCE/("SM_"+name+".obj"); mesh.save(path); icon(mesh,SOURCE/("T_Icon_"+name+"_01.png"))
        report["models"].append({"name":name,"vertices":len(mesh.vertices),"triangles":sum(len(face)-2 for face,_ in mesh.faces),"sha256":hashlib.sha256(path.read_bytes()).hexdigest()})
    (SOURCE/"SOURCE_MANIFEST.json").write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
    return report


def enum_value(cls, normalized):
    for name in dir(cls):
        if name.replace("_", "").upper() == normalized: return getattr(cls,name)
    raise RuntimeError("Missing enum "+normalized)


def author_material(material, palette=None):
    lib=unreal.MaterialEditingLibrary
    tag="AFTERSHOCK_CITY_DAMAGE_POWER_V2:"
    def node(key,cls,**props):
        found=[n for n in lib.get_material_expressions(material) if str(n.get_editor_property("desc"))==tag+key]
        if len(found)>1: raise RuntimeError("Duplicate tagged material node "+key)
        n=found[0] if found else lib.create_material_expression(material,cls)
        if n.get_class()!=cls.static_class(): raise RuntimeError("Node type mismatch "+key)
        n.set_editor_property("desc",tag+key)
        for name,value in props.items(): n.set_editor_property(name,value)
        return n
    def link(a,b,pin="",channel=""):
        if not lib.connect_material_expressions(a,channel,b,pin): raise RuntimeError("Material connection failed "+pin)
    def output(n,prop):
        if not lib.connect_material_property(n,"",prop): raise RuntimeError("Material output failed")
    old=lib.get_material_property_input_node(material,unreal.MaterialProperty.MP_BASE_COLOR)
    existing=[n for n in lib.get_material_expressions(material) if str(n.get_editor_property("desc"))==tag+"CracksChipsBrokenWindows"]
    if existing:
        old_inputs=list(lib.get_inputs_for_material_expression(material,existing[0]))
        names=list(lib.get_material_expression_input_names(existing[0]))
        old=old_inputs[names.index("Base")]
    if palette:
        old=node("Palette",unreal.MaterialExpressionTextureSampleParameter2D,parameter_name="ServicePalette",texture=palette)
    if not old: raise RuntimeError("Preserve source: base material must have a proven albedo input")
    position=node("Position",unreal.MaterialExpressionWorldPosition)
    values={}
    for label,index,default in (("Damage",0,0.),("Power",2,1.),("Operation",3,1.)):
        raw=node(label+"PerInstance",unreal.MaterialExpressionPerInstanceCustomData,data_index=index,const_default_value=default)
        interpolated=node(label+"Pixel",unreal.MaterialExpressionVertexInterpolator)
        link(raw,interpolated); values[label]=interpolated
    def custom(key,code,inputs):
        n=node(key,unreal.MaterialExpressionCustom,code=code,output_type=enum_value(unreal.CustomMaterialOutputType,"CMOTFLOAT3"))
        fields=[]
        for name in inputs:
            field=unreal.CustomInput(); field.set_editor_property("input_name",name); fields.append(field)
        n.set_editor_property("inputs",fields)
        for name,source in inputs.items(): link(source,n,name)
        return n
    # Damage is the actual normalized CurrentDamage. These are cosmetic surface
    # cues only; thresholds do not modify collision, loss, hazard or city state.
    color=custom("CracksChipsBrokenWindows",r"""
float d=saturate(Damage); float3 q=P*.025;
float fractureLine=abs(frac(q.z+sin(q.x*2.7)*.29+sin(q.y*3.1)*.21)-.5);
float aa=max(fwidth(fractureLine),.006);
float cracks=(1-smoothstep(.012,.028+aa,fractureLine))*saturate((d-.025)*3.8);
float h=frac(sin(dot(floor(P*.045),float3(12.9898,78.233,37.719)))*43758.5453);
float chipped=step(.50,d)*smoothstep(1-d*.45,1,h);
float soot=saturate((d-.20)*.75);
float window=step(Base.r*1.4,Base.b)*step(Base.r*1.25,Base.g)*(1-step(.20,Base.r));
float broken=step(.18,d)*step(1-d,h)*window;
float3 damaged=lerp(Base,float3(.025,.023,.021),saturate(cracks*.90+chipped*.8+soot*.55));
damaged=lerp(damaged,float3(.004,.006,.009),broken);
return damaged;
""",{"Base":old,"P":position,"Damage":values["Damage"]})
    emission=custom("PoweredUnbrokenWindows",r"""
float d=saturate(Damage);
float window=step(Base.r*1.4,Base.b)*step(Base.r*1.25,Base.g)*(1-step(.20,Base.r));
float h=frac(sin(dot(floor(P*.045),float3(12.9898,78.233,37.719)))*43758.5453);
float unbroken=1-step(.18,d)*step(1-d,h);
return window*unbroken*saturate(Power)*saturate(Operation)*float3(.30,.24,.13);
""",{"Base":old,"P":position,"Damage":values["Damage"],"Power":values["Power"],"Operation":values["Operation"]})
    output(color,unreal.MaterialProperty.MP_BASE_COLOR)
    output(emission,unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    if palette:
        rough=node("Roughness",unreal.MaterialExpressionConstant,r=.73); output(rough,unreal.MaterialProperty.MP_ROUGHNESS)
    lib.set_base_material_usage(material,unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES,True)
    lib.set_base_material_usage(material,unreal.MaterialUsage.MATUSAGE_GEOMETRY_COLLECTIONS,True)
    errors=lib.recompile_material(material)
    if errors: raise RuntimeError("City damage material compile failed: "+str(list(errors)))
    if not unreal.EditorAssetLibrary.save_loaded_asset(material): raise RuntimeError("Material save failed")
    return {"material":material.get_path_name(),"compile_errors":[],"instance_values":{"0":"CurrentDamage/100","1":"retrofit","2":"canonical power coverage","3":"canonical operation"},"visual_only":True}


def import_assets(report):
    tools=unreal.AssetToolsHelpers.get_asset_tools()
    def import_file(source,destination,name,options=None):
        task=unreal.AssetImportTask()
        for key,value in (("filename",str(source)),("destination_path",destination),("destination_name",name),("automated",True),("replace_existing",True),("save",True)):
            task.set_editor_property(key,value)
        if options: task.set_editor_property("options",options)
        tools.import_asset_tasks([task])
        asset=unreal.load_asset(destination+"/"+name)
        if not asset: raise RuntimeError("Asset import failed "+name)
        return asset
    palette=import_file(SOURCE/"T_ServicePalette.png","/Game/Buildings/Textures","T_ServicePalette")
    palette.set_editor_property("srgb",True)
    palette.set_editor_property("filter",unreal.TextureFilter.TF_NEAREST)
    palette.set_editor_property("mip_gen_settings",unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
    unreal.EditorAssetLibrary.save_loaded_asset(palette)
    path="/Game/Buildings/Materials/M_ServiceBuilding"
    material=unreal.load_asset(path)
    if not material:
        folder,name=path.rsplit("/",1); unreal.EditorAssetLibrary.make_directory(folder)
        material=tools.create_asset(name,folder,unreal.Material,unreal.MaterialFactoryNew())
    report["materials"]=[author_material(material,palette)]
    existing=unreal.load_asset("/Game/Buildings/Materials/M_Building_Base")
    if not existing: raise RuntimeError("Existing building master is missing")
    report["materials"].append(author_material(existing))
    for name in NAMES:
        options=unreal.FbxImportUI()
        options.set_editor_property("import_mesh",True)
        options.set_editor_property("import_materials",False)
        options.set_editor_property("import_textures",False)
        options.set_editor_property("automated_import_should_detect_type",False)
        options.set_editor_property("mesh_type_to_import",unreal.FBXImportType.FBXIT_STATIC_MESH)
        data=unreal.FbxStaticMeshImportData()
        data.set_editor_property("combine_meshes",True)
        data.set_editor_property("auto_generate_collision",True)
        data.set_editor_property("generate_lightmap_u_vs",False)
        data.set_editor_property("normal_import_method",unreal.FBXNormalImportMethod.FBXNIM_COMPUTE_NORMALS)
        options.set_editor_property("static_mesh_import_data",data)
        mesh=import_file(SOURCE/("SM_"+name+".obj"),"/Game/Buildings/Meshes/Services","SM_"+name,options)
        if not isinstance(mesh,unreal.StaticMesh): raise RuntimeError("Expected one StaticMesh "+name)
        mesh.set_material(0,material)
        nanite=mesh.get_editor_property("nanite_settings"); nanite.set_editor_property("enabled",False); mesh.set_editor_property("nanite_settings",nanite)
        unreal.EditorAssetLibrary.save_loaded_asset(mesh)
        image=import_file(SOURCE/("T_Icon_"+name+"_01.png"),"/Game/UI/Icons/Buildings","T_Icon_"+name+"_01")
        image.set_editor_property("lod_group",unreal.TextureGroup.TEXTUREGROUP_UI)
        image.set_editor_property("mip_gen_settings",unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
        image.set_editor_property("compression_settings",unreal.TextureCompressionSettings.TC_EDITOR_ICON)
        unreal.EditorAssetLibrary.save_loaded_asset(image)
        bounds=mesh.get_bounds(); dimensions=[round(v*2,2) for v in (bounds.box_extent.x,bounds.box_extent.y,bounds.box_extent.z)]
        next(item for item in report["models"] if item["name"]==name).update({"asset":mesh.get_path_name(),"dimensions_cm":dimensions,"icon":image.get_path_name(),"material_slots":len(mesh.get_editor_property("static_materials"))})
    report_path=ROOT/"Saved"/"PresentationValidation"/"city_services_assets.json"; report_path.parent.mkdir(parents=True,exist_ok=True)
    report_path.write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
    unreal.log_warning("AFTERSHOCK_CITY_SERVICES_READY models=15 originalIcons=15 materials=2 source_manifest="+str(SOURCE/"SOURCE_MANIFEST.json"))


def main():
    report=generate_sources()
    if unreal is None:
        print("Generated15 original service models, palette and orthographic icons in "+str(SOURCE)); return
    import_assets(report)


if __name__ == "__main__":
    main()
