"""Bridge normalized ISM damage to the existing CPD-driven building material.

BuildingManager writes per-instance custom data index 0 in [0, 1]. The original
master reads per-component CPD CrackAmount index 0 in [0, 100] and divides by 100.
This additive patch feeds Max(legacy CPD, Saturate(instance damage) * 100) into
that existing normalization node. It leaves the palette, crack vertex mask, WPO
and other material outputs intact. No node/package deletion; reruns reuse tags.

Execute inside UE 5.8 using -run=pythonscript -script=<absolute script path>.
"""

import json
import os

import unreal


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MATERIAL_PATH = "/Game/Buildings/Materials/M_Building_Base"
TAG = "AFTERSHOCK_INSTANCE_DAMAGE_V1:"
LIB = unreal.MaterialEditingLibrary


def inputs(material, expression):
    return list(LIB.get_inputs_for_material_expression(material, expression))


def path(expression):
    return expression.get_path_name() if expression else None


def graph_snapshot(material):
    nodes = []
    for expression in LIB.get_material_expressions(material):
        names = list(LIB.get_material_expression_input_names(expression))
        connections = inputs(material, expression)
        nodes.append({"node": path(expression), "class": expression.get_class().get_name(),
                      "description": str(expression.get_editor_property("desc")),
                      "inputs": [{"pin": names[index] if index < len(names) else str(index),
                                  "source": path(source)} for index, source in enumerate(connections)]})
    return nodes


def output_snapshot(material):
    properties = {"base_color": unreal.MaterialProperty.MP_BASE_COLOR,
                  "normal": unreal.MaterialProperty.MP_NORMAL,
                  "roughness": unreal.MaterialProperty.MP_ROUGHNESS,
                  "world_position_offset": unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET}
    return {label: path(LIB.get_material_property_input_node(material, prop))
            for label, prop in properties.items()}


def tagged(material, name):
    matches = [expression for expression in LIB.get_material_expressions(material)
               if str(expression.get_editor_property("desc")) == TAG + name]
    if len(matches) > 1:
        raise RuntimeError("Duplicate tagged bridge nodes; preserve for review: " + name)
    return matches[0] if matches else None


def create_or_reuse(material, cls, label, **properties):
    expression = tagged(material, label)
    if expression and expression.get_class() != cls.static_class():
        raise RuntimeError("Unexpected node type for bridge tag " + label)
    if not expression:
        expression = LIB.create_material_expression(material, cls)
    if not expression:
        raise RuntimeError("Cannot create bridge node " + label)
    expression.set_editor_property("desc", TAG + label)
    for key, value in properties.items():
        expression.set_editor_property(key, value)
    return expression


def connect(source, target, pin=""):
    if not LIB.connect_material_expressions(source, "", target, pin):
        raise RuntimeError("Cannot connect damage bridge pin " + pin)


def main():
    material = unreal.load_asset(MATERIAL_PATH)
    if not isinstance(material, unreal.Material):
        raise RuntimeError("Missing building master material")
    expressions = list(LIB.get_material_expressions(material))
    damage_parameters = [expression for expression in expressions
                         if expression.get_class().get_name() == "MaterialExpressionScalarParameter"
                         and str(expression.get_editor_property("parameter_name")) == "CrackAmount"]
    if len(damage_parameters) != 1:
        raise RuntimeError("Expected one existing CrackAmount scalar parameter")
    legacy_damage = damage_parameters[0]
    if not legacy_damage.get_editor_property("use_custom_primitive_data"):
        raise RuntimeError("Existing CrackAmount is not CPD; audit contract before modifying")
    if legacy_damage.get_editor_property("primitive_data_index") != 0:
        raise RuntimeError("Existing CrackAmount is not CPD index 0")

    old_max = tagged(material, "MaxLegacyAndInstancePercent")
    normalization_matches = []
    for expression in expressions:
        if expression.get_class().get_name() != "MaterialExpressionMultiply":
            continue
        connected = inputs(material, expression)
        sources = [legacy_damage] + ([old_max] if old_max else [])
        source_indices = [index for index, item in enumerate(connected) if item in sources]
        percent_constants = [item for item in connected if item
                             and item.get_class().get_name() == "MaterialExpressionConstant"
                             and abs(item.get_editor_property("r") - 0.01) < 0.00001]
        if len(source_indices) == 1 and len(percent_constants) == 1:
            normalization_matches.append((expression, source_indices[0]))
    if len(normalization_matches) != 1:
        raise RuntimeError("Cannot uniquely prove existing percent-to-ratio connection; preserve graph")
    normalization, source_index = normalization_matches[0]
    input_names = list(LIB.get_material_expression_input_names(normalization))
    if source_index >= len(input_names) or str(input_names[source_index]) not in ("A", "B"):
        raise RuntimeError("Unexpected normalization pin names: " + str(input_names))
    normalization_pin = str(input_names[source_index])
    before = graph_snapshot(material)
    outputs_before = output_snapshot(material)
    geometry_usage_before = LIB.has_material_usage(material, unreal.MaterialUsage.MATUSAGE_GEOMETRY_COLLECTIONS)

    instance = create_or_reuse(material, unreal.MaterialExpressionPerInstanceCustomData,
                               "NormalizedInstanceDamage", data_index=0, const_default_value=0.0)
    clamped = create_or_reuse(material, unreal.MaterialExpressionSaturate, "ClampInstanceRatio")
    scale = create_or_reuse(material, unreal.MaterialExpressionScalarParameter,
                            "InstancePercentScale", parameter_name="AftershockInstanceDamagePercentScale",
                            default_value=100.0)
    percent = create_or_reuse(material, unreal.MaterialExpressionMultiply, "InstanceRatioToPercent")
    combined = create_or_reuse(material, unreal.MaterialExpressionMax, "MaxLegacyAndInstancePercent")
    connect(instance, clamped)
    connect(clamped, percent, "A")
    connect(scale, percent, "B")
    connect(legacy_damage, combined, "A")
    connect(percent, combined, "B")
    connect(combined, normalization, normalization_pin)

    # Preserve both render paths: ISM buildings and non-instanced/GC legacy CPD.
    LIB.set_base_material_usage(material, unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES, True)
    LIB.set_base_material_usage(material, unreal.MaterialUsage.MATUSAGE_GEOMETRY_COLLECTIONS, True)
    if outputs_before != output_snapshot(material):
        raise RuntimeError("A material output unexpectedly changed; do not save")
    if combined not in inputs(material, normalization):
        raise RuntimeError("Damage normalization bridge did not connect")
    if legacy_damage not in inputs(material, combined) or percent not in inputs(material, combined):
        raise RuntimeError("Legacy and instance damage must both remain connected")
    errors = LIB.recompile_material(material)
    if errors:
        raise RuntimeError("Building damage bridge shader compile failed: " + str(list(errors)))
    if not unreal.EditorAssetLibrary.save_loaded_asset(material):
        raise RuntimeError("Could not save building damage bridge")

    report = {"material": MATERIAL_PATH, "bridge_version": 1, "compile_errors": [],
              "legacy_contract": {"parameter": "CrackAmount", "cpd_index": 0, "range": [0, 100]},
              "instance_contract": {"data_index": 0, "range": [0, 1]},
              "formula": "max(CPD_CrackAmount, saturate(PerInstanceData0)*100)*0.01",
              "normalization_node": path(normalization), "normalization_input": normalization_pin,
              "outputs_preserved": outputs_before,
              "geometry_collections_usage_before": geometry_usage_before,
              "geometry_collections_usage_after": LIB.has_material_usage(material, unreal.MaterialUsage.MATUSAGE_GEOMETRY_COLLECTIONS),
              "tagged_nodes": [path(instance), path(clamped), path(scale), path(percent), path(combined)],
              "contract_examples_not_render_measurements": [
                  {"cpd_percent": 0, "instance_ratio": 0.55, "normalized_damage": 0.55},
                  {"cpd_percent": 45, "instance_ratio": 0, "normalized_damage": 0.45}],
              "graph_before": before, "graph_after": graph_snapshot(material)}
    report_path = os.path.join(ROOT, "Saved", "PresentationValidation", "building_instance_damage_bridge.json")
    os.makedirs(os.path.dirname(report_path), exist_ok=True)
    with open(report_path, "w", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
    unreal.log_warning("AFTERSHOCK_INSTANCE_DAMAGE_BRIDGE_READY material={} nodes={} normalization={} pin={} report={}".format(
        MATERIAL_PATH, len(report["tagged_nodes"]), path(normalization), normalization_pin, report_path))


main()
