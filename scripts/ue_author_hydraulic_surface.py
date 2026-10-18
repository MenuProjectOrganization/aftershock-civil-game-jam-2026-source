"""Prepare one original hydraulic material; run ONLY in a root-owned UE session.

No external art/code download. Only this owned material graph is updated;
an unexpected existing graph at the path aborts. A recipe update is not proof
that its material asset has been reauthored, compiled or rendered successfully.
The dynamic float texture is supplied by the actor, never imported as an asset.
"""
import unreal

PATH = "/Game/Environment/Water/M_HydraulicSurface"
OWNED = "AftershockHydraulic:v1:"
OPTICS_RECIPE = "hydraulic-optics-v5-subcell-shore-freehead"
L = unreal.MaterialEditingLibrary


def props(obj, **values):
    for key, value in values.items():
        obj.set_editor_property(key, value)
    return obj


def expressions(material):
    return list(L.get_material_expressions(material))


def node(material, label, cls, **values):
    matches = [e for e in expressions(material) if e.get_editor_property("desc") == OWNED + label]
    if len(matches) > 1:
        raise RuntimeError("Duplicate owned expression: " + label)
    n = matches[0] if matches else L.create_material_expression(material, cls, -1000, 0)
    if n.get_class() != cls.static_class():
        raise RuntimeError("Unexpected owned expression class: " + label)
    props(n, desc=OWNED + label, **values)
    return n


def scalar(m, name, default):
    return node(m, name, unreal.MaterialExpressionScalarParameter,
                parameter_name=name, default_value=default)


def connect(source, dest, name="", output=""):
    if not L.connect_material_expressions(source, output, dest, name):
        raise RuntimeError("Failed material connection: " + name)


def custom(m, label, code, inputs, output=unreal.CustomMaterialOutputType.CMOT_FLOAT4):
    n = node(m, label, unreal.MaterialExpressionCustom)
    entries = []
    for name, source in inputs.items():
        item = unreal.CustomInput()
        item.set_editor_property("input_name", name)
        entries.append(item)
    notify = unreal.PropertyAccessChangeNotifyMode.NEVER
    n.set_editor_property("inputs", entries, notify_mode=notify)
    for name, source in inputs.items():
        connect(source, n, name)
    n.set_editor_property("output_type", output, notify_mode=notify)
    n.set_editor_property("code", code, notify_mode=notify)
    return n


def output(m, source, property):
    if not L.connect_material_property(source, "", property):
        raise RuntimeError("Failed material property connection: " + str(property))


def main():
    # Read dependencies + validate the existing path before mutating anything.
    black = unreal.load_asset("/Engine/Functions/Engine_MaterialFunctions02/PivotPainter2/Black_1x1_EXR_Texture")
    if not isinstance(black, unreal.Texture2D) or black.get_editor_property("srgb"):
        raise RuntimeError("Expected existing linear engine EXR placeholder; no asset was changed")
    m = unreal.load_asset(PATH)
    if m:
        if not isinstance(m, unreal.Material):
            raise RuntimeError("Hydraulic target exists but is not a base Material")
        existing = expressions(m)
        if not existing or any(not e.get_editor_property("desc").startswith(OWNED) for e in existing):
            raise RuntimeError("Target contains unexpected/unowned art graph; preserve it and stop")
    else:
        folder, name = PATH.rsplit("/", 1)
        unreal.EditorAssetLibrary.make_directory(folder)
        m = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, folder, unreal.Material, unreal.MaterialFactoryNew())
    if not m:
        raise RuntimeError("Failed creating original hydraulic surface material")
    props(m, blend_mode=unreal.BlendMode.BLEND_TRANSLUCENT,
          shading_model=unreal.MaterialShadingModel.MSM_DEFAULT_LIT,
          two_sided=True, tangent_space_normal=False,
          translucency_lighting_mode=unreal.TranslucencyLightingMode.TLM_SURFACE_PER_PIXEL_LIGHTING,
          screen_space_reflections=True)
    field = node(m, "HydraulicField", unreal.MaterialExpressionTextureObjectParameter,
                 parameter_name="HydraulicField", texture=black,
                 sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    # Default EXR is a linear placeholder; runtime PF_G8 reads its red channel.
    # Native proof must verify normalized R8 decoding of exact bytes 0/1/2/4/8/15.
    mask = node(m, "HydraulicConnectivity", unreal.MaterialExpressionTextureObjectParameter,
                parameter_name="HydraulicConnectivity", texture=black,
                sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    uv = node(m, "FieldUV", unreal.MaterialExpressionTextureCoordinate, coordinate_index=0)
    size = scalar(m, "HydraulicTextureSize", 256.)
    window = node(m, "HydraulicWindowSize", unreal.MaterialExpressionVectorParameter,
                  parameter_name="HydraulicWindowSize", default_value=unreal.LinearColor(1, 1, 0, 0))
    origin = scalar(m, "HydraulicOriginZCm", 0.)
    cell = scalar(m, "HydraulicCellSizeCm", 1000.)
    wind = node(m, "HydraulicWind", unreal.MaterialExpressionVectorParameter,
                parameter_name="HydraulicWind", default_value=unreal.LinearColor(1, 0, 0, 0))
    position = node(m, "WorldPositionWithoutWPO", unreal.MaterialExpressionWorldPosition,
                    world_position_shader_offset=unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)
    time = node(m, "VisualFlowTime", unreal.MaterialExpressionTime, ignore_pause=False)
    normal_quality = scalar(m, "GlobalWaterNormalQuality", .065)
    foam_quality = scalar(m, "GlobalWaterFoamQuality", .85)
    scalar(m, "HydraulicPhysicalSeconds", 0.)  # Named telemetry; not a 24s wipe timer.
    scalar(m, "HydraulicSchemaVersion", 1.)
    read = custom(m, "ExactPointState", r"""
float2 p=clamp(floor(UV*Size),float2(0,0),max(Window.xy-1.,0.));
return Texture2DSampleLevel(Field,FieldSampler,(p+.5)/Size,0);
""", {"Field": field, "UV": uv, "Size": size, "Window": window})
    surface_state = custom(m, "ConnectedSurfaceState", r"""
// Exact canonical point samples remain unchanged in the uploaded texture.
// Display reconstruction uses the exact centre, connected face midpoints and
// the wet connected component touching each corner. Owners on an open face
// therefore use the SAME midpoint and corner, including a three-wet-cell L.
// A diagonal cell without any mutually open cardinal path is never joined.
float2 p=clamp(floor(UV*Size),float2(0,0),max(Window.xy-1.,0.));
float2 f=saturate(UV*Size-p);
// UVs are inset to preserve owner identity. Snap only those inset endpoints
// back to the exact shared position so adjacent open faces get equal heads.
f=lerp(f,float2(0,0),1.-step(.001,f));
f=lerp(f,float2(1,1),step(.999,f));
float2 delta=f-.5,d=step(0.,delta)*2.-1.;
float2 px=p+float2(d.x,0),py=p+float2(0,d.y),pd=p+d;
bool ix=all(px>=0.)&&all(px<Window.xy),iy=all(py>=0.)&&all(py<Window.xy),id=all(pd>=0.)&&all(pd<Window.xy);
float4 sx=ix?Texture2DSampleLevel(Field,FieldSampler,(px+.5)/Size,0):State;
float4 sy=iy?Texture2DSampleLevel(Field,FieldSampler,(py+.5)/Size,0):State;
float4 sd=id?Texture2DSampleLevel(Field,FieldSampler,(pd+.5)/Size,0):State;
uint ma=(uint)round(Texture2DSampleLevel(Mask,MaskSampler,(p+.5)/Size,0).r*255.);
uint mx=ix?(uint)round(Texture2DSampleLevel(Mask,MaskSampler,(px+.5)/Size,0).r*255.):15u;
uint my=iy?(uint)round(Texture2DSampleLevel(Mask,MaskSampler,(py+.5)/Size,0).r*255.):15u;
uint md=id?(uint)round(Texture2DSampleLevel(Mask,MaskSampler,(pd+.5)/Size,0).r*255.):15u;
uint bx=d.x>0.?2u:8u,ox=d.x>0.?8u:2u,by=d.y>0.?4u:1u,oy=d.y>0.?1u:4u;
bool wet=State.y>.00001;
// Rain can wet a high bank while the river head is still below its bed.
// Nonzero depth on both sides alone must NOT pull that river surface up the
// bank. Blend only columns sharing a real vertical water interval. This reads
// canonical eta/bed/depth; it never changes hydraulic flux or storage.
bool cx=wet&&ix&&sx.y>.00001&&min(State.x,sx.x)>max(State.x-State.y,sx.x-sx.y)+.00001&&((ma&bx)==0u)&&((mx&ox)==0u);
bool cy=wet&&iy&&sy.y>.00001&&min(State.x,sy.x)>max(State.x-State.y,sy.x-sy.y)+.00001&&((ma&by)==0u)&&((my&oy)==0u);
bool xd=ix&&id&&sx.y>.00001&&sd.y>.00001&&min(sx.x,sd.x)>max(sx.x-sx.y,sd.x-sd.y)+.00001&&((mx&by)==0u)&&((md&oy)==0u);
bool yd=iy&&id&&sy.y>.00001&&sd.y>.00001&&min(sy.x,sd.x)>max(sy.x-sy.y,sd.x-sd.y)+.00001&&((my&bx)==0u)&&((md&ox)==0u);
// Four nodes / four reciprocal face edges: three bounded expansions find the
// complete local component. This also reaches the third wet cell around an
// L corner, without crossing a blocked edge or joining isolated diagonals.
bool r0=wet,r1=false,r2=false,r3=false;
[unroll] for(int k=0;k<3;++k)
{
    bool n1=r1||(r0&&cx)||(r3&&xd);
    bool n2=r2||(r0&&cy)||(r3&&yd);
    bool n3=r3||(r1&&xd)||(r2&&yd);
    r1=n1;r2=n2;r3=n3;
}
float4 cornerState=State;float count=1.;
if(r1){cornerState+=sx;count+=1.;}
if(r2){cornerState+=sy;count+=1.;}
if(r3){cornerState+=sd;count+=1.;}
cornerState/=count;
float4 faceX=cx?(State+sx)*.5:State,faceY=cy?(State+sy)*.5:State;
float2 t=saturate(abs(delta)*2.);
return lerp(lerp(State,faceX,t.x),lerp(faceY,cornerState,t.x),t.y);
""", {"Field": field, "Mask": mask, "UV": uv, "Size": size, "Window": window, "State": read})
    shore_support = custom(m, "SubcellShoreSupport", r"""
// Reconstruct a SUBCELL visual shoreline from canonical depth support. No
// texture, water volume, hazard footprint or gameplay result is written.
float2 p=clamp(floor(UV*Size),0.,max(Window.xy-1.,0.));
float2 f=saturate(UV*Size-p),d=step(.5,f)*2.-1.,t=abs(f-.5)*2.;
float2 px=p+float2(d.x,0),py=p+float2(0,d.y),pd=p+d;
bool ix=all(px>=0.)&&all(px<Window.xy),iy=all(py>=0.)&&all(py<Window.xy),id=all(pd>=0.)&&all(pd<Window.xy);
float4 sx=ix?Texture2DSampleLevel(Field,FieldSampler,(px+.5)/Size,0):PointState;
float4 sy=iy?Texture2DSampleLevel(Field,FieldSampler,(py+.5)/Size,0):PointState;
float4 sd=id?Texture2DSampleLevel(Field,FieldSampler,(pd+.5)/Size,0):PointState;
uint ma=(uint)round(Texture2DSampleLevel(Mask,MaskSampler,(p+.5)/Size,0).r*255.);
uint mx=ix?(uint)round(Texture2DSampleLevel(Mask,MaskSampler,(px+.5)/Size,0).r*255.):15u;
uint my=iy?(uint)round(Texture2DSampleLevel(Mask,MaskSampler,(py+.5)/Size,0).r*255.):15u;
uint md=id?(uint)round(Texture2DSampleLevel(Mask,MaskSampler,(pd+.5)/Size,0).r*255.):15u;
uint bx=d.x>0.?2u:8u,ox=d.x>0.?8u:2u,by=d.y>0.?4u:1u,oy=d.y>0.?1u:4u;
bool ex=ix&&((ma&bx)==0u)&&((mx&ox)==0u),ey=iy&&((ma&by)==0u)&&((my&oy)==0u);
bool diag=ex&&ey&&id&&((mx&by)==0u)&&((md&oy)==0u)&&((my&bx)==0u)&&((md&ox)==0u);
// SI metres: 2..6cm is a transparent thin film, not an opaque whole-city sheet.
float q0=smoothstep(.02,.06,PointState.y),qx=ex?smoothstep(.02,.06,sx.y):0.;
float qy=ey?smoothstep(.02,.06,sy.y):0.,qd=diag?smoothstep(.02,.06,sd.y):0.;
if(qx+qy<.00001)qd=0.; // isolated diagonal never creates a corner bridge
if(q0+qd<.00001&&qx>0.&&qy>0.){if(qx>=qy)qy=0.;else qx=0.;}
float sum=q0+qx+qy+qd;
float2 corner=sum>.00001?(PointState.xy*q0+sx.xy*qx+sy.xy*qy+sd.xy*qd)/sum:PointState.xy;
// A dry support centre has zero coverage. Its invisible geometric support
// stays at the neighbouring FREE HEAD, so the real opaque bank clips it;
// interpolating toward the high dry-bank eta would recreate square notches.
float2 centre=q0>.00001?PointState.xy:corner;
float2 faceX=q0+qx>.00001?(PointState.xy*q0+sx.xy*qx)/(q0+qx):centre;
float2 faceY=q0+qy>.00001?(PointState.xy*q0+sy.xy*qy)/(q0+qy):centre;
// Keep real disconnected water columns separated at their face midpoint.
if(q0>0.&&qx>0.&&min(PointState.x,sx.x)<=max(PointState.x-PointState.y,sx.x-sx.y)+.00001)faceX=PointState.xy;
if(q0>0.&&qy>0.&&min(PointState.x,sy.x)<=max(PointState.x-PointState.y,sy.x-sy.y)+.00001)faceY=PointState.xy;
float2 support=lerp(lerp(centre,faceX,t.x),lerp(faceY,corner,t.x),t.y);
float coverage=lerp(lerp(q0,(q0+qx)*.5,t.x),lerp((q0+qy)*.5,sum*.25,t.x),t.y);
return float4(support,coverage,0.);
""", {"Field": field, "Mask": mask, "UV": uv, "Size": size, "Window": window, "PointState": read})
    optical_state = custom(m, "SubcellOpticalState", "return float4(Shore.xy,State.zw);",
                           {"Shore": shore_support, "State": surface_state})
    wpo = custom(m, "ActualHeadAndBoundedWind", r"""
float windStrength=saturate(Wind.z);
float2 wd=dot(Wind.xy,Wind.xy)>1e-6?normalize(Wind.xy):float2(1,0);
float ripple=sin(dot(Position.xy*.01,wd)*5.2-Time*1.8)*.65;
ripple*=windStrength*saturate(Quality/.08)*saturate(State.y/.08);
return float3(0,0,Origin+State.x*100.-Position.z+ripple);
""", {"State": optical_state, "Origin": origin, "Position": position, "Time": time, "Wind": wind, "Quality": normal_quality},
       unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    output(m, wpo, unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
    normal_foam = custom(m, "PhysicalFlowNormalAndFoam", r"""
float2 p=clamp(floor(UV*Size),float2(0,0),max(Window.xy-1.,0.));
float2 pl=p+float2(-1,0),pr=p+float2(1,0),pb=p+float2(0,-1),pt=p+float2(0,1);
bool il=pl.x>=0.,ir=pr.x<Window.x,ib=pb.y>=0.,it=pt.y<Window.y;
float4 l=il?Texture2DSampleLevel(Field,FieldSampler,(pl+.5)/Size,0):State;
float4 r=ir?Texture2DSampleLevel(Field,FieldSampler,(pr+.5)/Size,0):State;
float4 b=ib?Texture2DSampleLevel(Field,FieldSampler,(pb+.5)/Size,0):State;
float4 t=it?Texture2DSampleLevel(Field,FieldSampler,(pt+.5)/Size,0):State;
uint ma=(uint)round(Texture2DSampleLevel(Mask,MaskSampler,(p+.5)/Size,0).r*255.);
uint ml=il?(uint)round(Texture2DSampleLevel(Mask,MaskSampler,(pl+.5)/Size,0).r*255.):15u;
uint mr=ir?(uint)round(Texture2DSampleLevel(Mask,MaskSampler,(pr+.5)/Size,0).r*255.):15u;
uint mb=ib?(uint)round(Texture2DSampleLevel(Mask,MaskSampler,(pb+.5)/Size,0).r*255.):15u;
uint mt=it?(uint)round(Texture2DSampleLevel(Mask,MaskSampler,(pt+.5)/Size,0).r*255.):15u;
bool wet=State.y>.00001;
bool cl=wet&&il&&l.y>.00001&&min(State.x,l.x)>max(State.x-State.y,l.x-l.y)+.00001&&((ma&8u)==0u)&&((ml&2u)==0u);
bool cr=wet&&ir&&r.y>.00001&&min(State.x,r.x)>max(State.x-State.y,r.x-r.y)+.00001&&((ma&2u)==0u)&&((mr&8u)==0u);
bool cb=wet&&ib&&b.y>.00001&&min(State.x,b.x)>max(State.x-State.y,b.x-b.y)+.00001&&((ma&1u)==0u)&&((mb&4u)==0u);
bool ct=wet&&it&&t.y>.00001&&min(State.x,t.x)>max(State.x-State.y,t.x-t.y)+.00001&&((ma&4u)==0u)&&((mt&1u)==0u);
l=cl?l:State;r=cr?r:State;b=cb?b:State;t=ct?t:State;
// Gradient and optical advection never smooth across a canonical blocked edge.
float el=l.x,er=r.x,eb=b.x,et=t.x;
float dx=max(.01,Cell*.01),h=max(State.y,.00001);
float2 slope=float2(er-el,et-eb)/(2.*dx);
float2 v=State.zw;
// Two phase flow mapping avoids speed-change phase pops: current canonical
// velocity is the only flow speed/direction. This is optical, not a new solver.
float a=fmod(Time,2.),c=fmod(Time+1.,2.);
float wa=1.-abs(a-1.),wc=1.-abs(c-1.);
float2 pa=Position.xy*.01-v*a,pc=Position.xy*.01-v*c;
// Optical normal detail only: keep the exact local State.zw advection above.
// The former ~1m parallel sine bands alias at the city camera. Lower-frequency
// phase-warped detail fades before it becomes subpixel, leaving the actual
// hydraulic head slope intact. Derivatives are evaluated only in pixel optics.
float2 worldM=Position.xy*.01;
float footprintM=max(length(ddx(worldM)),length(ddy(worldM)));
// The real city-camera capture resolved the previous 2..3m sine structure as
// strong parallel bands. Fade optical microdetail before a pixel covers .22m;
// the actual canonical surface slope/velocity remains present at every zoom.
float detailFilter=1.-smoothstep(.05,.22,footprintM);
float warpA=sin(dot(pa,float2(.27,-.19)))*.75;
float warpC=sin(dot(pc,float2(.27,-.19)))*.75;
float2 detailA=float2(sin(pa.x*1.85+pa.y*.63+warpA),cos(pa.y*2.07-pa.x*.47-warpA));
float2 detailC=float2(sin(pc.x*1.85+pc.y*.63+warpC),cos(pc.y*2.07-pc.x*.47-warpC));
float speed=length(v),detailWeight=saturate(speed/.4+Wind.z*.25);
float2 detail=(detailA*wa+detailC*wc)*Quality*.25*detailWeight*saturate(h/.04)*detailFilter;
float froude=speed/sqrt(9.80665*max(h,.02));
float shear=(length(r.zw-l.zw)+length(t.zw-b.zw))/(2.*dx);
float turbulence=saturate((froude-.55)*.4+shear*.12);
float shore=(1.-smoothstep(.02,.18,h))*smoothstep(.002,.015,h);
float breakup=.72+.28*sin(pa.x*1.7-pa.y*1.1+warpA)*detailFilter;
float foam=saturate((turbulence*.3+shore*.10)*breakup*FoamQuality);
return float4(normalize(float3(-slope+detail,1)),foam);
""", {"Field": field, "Mask": mask, "State": optical_state, "UV": uv, "Size": size, "Window": window,
       "Cell": cell, "Position": position, "Time": time, "Wind": wind,
       "Quality": normal_quality, "FoamQuality": foam_quality})
    normal = node(m, "WorldNormalRGB", unreal.MaterialExpressionComponentMask, r=True, g=True, b=True, a=False)
    connect(normal_foam, normal)
    output(m, normal, unreal.MaterialProperty.MP_NORMAL)
    optics = custom(m, "ThicknessOpticsAndWetMask", r"""
float h=max(State.y,0.);
// Art-only absorption/palette in metres. Subcell shore support can cross an
// open face; the dry centre stays invisible and actual opaque ground clips it.
float absorption=1.-exp(-h/.85);
float3 shallow=float3(.065,.245,.205),deep=float3(.010,.055,.110);
float3 colour=lerp(shallow,deep,absorption);
colour=lerp(colour,float3(.62,.66,.63),saturate(NormalFoam.w));
float alpha=(1.-exp(-h*2.))* .88;
// Centre/face/corner support rounds cell corners and may extend only a
// subcell margin. A dry centre is zero; opaque ground rejects submerged support.
alpha*=smoothstep(.40,.60,Shore.z);
return float4(colour,alpha);
""", {"State": optical_state, "Shore": shore_support, "NormalFoam": normal_foam})
    rgb = node(m, "WaterRGB", unreal.MaterialExpressionComponentMask, r=True, g=True, b=True, a=False)
    alpha = node(m, "WaterAlpha", unreal.MaterialExpressionComponentMask, r=False, g=False, b=False, a=True)
    connect(optics, rgb); connect(optics, alpha)
    output(m, rgb, unreal.MaterialProperty.MP_BASE_COLOR)
    fade = node(m, "ActualOpaqueContact", unreal.MaterialExpressionDepthFade, fade_distance_default=18., opacity_default=1.)
    connect(alpha, fade, "Opacity"); output(m, fade, unreal.MaterialProperty.MP_OPACITY)
    output(m, scalar(m, "HydraulicWaterRoughness", .24), unreal.MaterialProperty.MP_ROUGHNESS)
    output(m, scalar(m, "WaterSpecular", .5), unreal.MaterialProperty.MP_SPECULAR)
    output(m, scalar(m, "WaterMetallic", 0.), unreal.MaterialProperty.MP_METALLIC)
    errors = L.recompile_material(m)
    if errors:
        raise RuntimeError("Hydraulic material compile errors: " + str(errors))
    if not unreal.EditorAssetLibrary.save_loaded_asset(m):
        raise RuntimeError("Failed saving " + PATH)
    unreal.log_warning("AFTERSHOCK_HYDRAULIC_MATERIAL_READY path={} opticsRecipe={} data=RGBA32F_etaRelativeM_depthM_uMps_vMps connectivity=PF_G8_N1E2S4W8 exactPointTextureUnchanged=1 cellLocalPatches=1 wetCentreHeadExact=1 dryCentreInvisible=1 connectedCentreFaceCornerDisplay=1 sharedFaceOpenBothSides=1 isolatedDiagonalBridge=0 localVelocityAdvection=1 opticalDetailPixelFiltered=1 subcellMarginMayCrossOpenFace=1 actualOpaqueGroundIntersection=1 full3DCFD=0 eventEnvelope=0 continuousPersistent=1 externalDownloads=0 nativeRHIProofPending=1".format(PATH, OPTICS_RECIPE))


if __name__ == "__main__":
    main()
