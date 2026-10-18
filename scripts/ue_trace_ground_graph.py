"""Read-only native material reachability diagnostic. Root executes in Unreal.

No expression, connection, parameter, shader compilation or asset save writes.
Writes only a Saved JSON report. Missing APIs/data fail explicitly.
"""
from pathlib import Path
import hashlib
import json
import re
import unreal

L = unreal.MaterialEditingLibrary
ROOT = Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())).resolve()
STYLE = 'AFTERSHOCK_REGIONAL_STYLE_V1:'
GLOBAL = 'AFTERSHOCK_GLOBAL_REGIONAL_COLOUR_V1:'
MATERIALS = ('/Game/Environment/Materials/Presentation/M_PresentationGroundPBR',
             '/Game/Environment/Materials/Presentation/M_AftershockRegionalBlend')
PROPERTIES = {'BaseColor': unreal.MaterialProperty.MP_BASE_COLOR,
              'Normal': unreal.MaterialProperty.MP_NORMAL,
              'Roughness': unreal.MaterialProperty.MP_ROUGHNESS}


def describe(node):
    if node is None:
        return None
    row = {'path': node.get_path_name(), 'class': node.get_class().get_name(),
           'desc': str(node.get_editor_property('desc'))}
    if row['class'] in ('MaterialExpressionTextureObjectParameter', 'MaterialExpressionTextureSampleParameter2D',
                        'MaterialExpressionScalarParameter', 'MaterialExpressionVectorParameter'):
        row['parameter_name'] = str(node.get_editor_property('parameter_name'))
    if row['class'] == 'MaterialExpressionScalarParameter':
        row['default_value'] = float(node.get_editor_property('default_value'))
    if row['class'] in ('MaterialExpressionTextureObjectParameter', 'MaterialExpressionTextureSampleParameter2D'):
        texture = node.get_editor_property('texture')
        row['default_texture'] = texture.get_path_name() if texture else None
    if row['class'] == 'MaterialExpressionCustom':
        code = str(node.get_editor_property('code'))
        row['code_sha256'] = hashlib.sha256(code.encode('utf-8')).hexdigest()
        row['refinement_v1'] = '// AS_GROUND_REFINEMENT_V1:COMMON' in code
        row['microvariation_v2'] = '// AS_GROUND_MICROVARIATION_V2' in code
        row['stochastic_v3'] = '// AS_GROUND_STOCHASTIC_V3' in code
        row['environment_normal_quality'] = 'AS_ENVIRONMENT_NORMAL_QUALITY' in code
    return row


def inputs(material, node):
    names = [str(name) for name in L.get_material_expression_input_names(node)]
    values = list(L.get_inputs_for_material_expression(material, node))
    if len(names) != len(values):
        raise RuntimeError('Input iterator/name length mismatch: ' + node.get_path_name())
    return [(index, name, values[index]) for index, name in enumerate(names)]


def inspect(material):
    nodes = list(L.get_material_expressions(material))
    by_path = {node.get_path_name(): node for node in nodes}
    if len(by_path) != len(nodes) or len(nodes) > 10000:
        raise RuntimeError('Ambiguous or over-limit expression inventory')
    targets = {}
    for label in PROPERTIES:
        found = [node for node in nodes if str(node.get_editor_property('desc')) == STYLE + 'Wrap' + label]
        targets[label] = found
    rows, all_edges = {}, {}
    for label, prop in PROPERTIES.items():
        entry = L.get_material_property_input_node(material, prop)
        target = targets[label][0] if len(targets[label]) == 1 else None
        visited, active, cycles, paths = set(), set(), [], {}

        def dfs(node, chain):
            if node is None:
                return
            key = node.get_path_name()
            if key in active:
                parent = next((part['path'] for part in reversed(chain) if 'path' in part), None)
                cycles.append({'from': parent, 'to': key})
                return
            if key in visited:
                return
            if len(visited) > 10000:
                raise RuntimeError('DFS node limit exceeded')
            visited.add(key)
            active.add(key)
            paths[key] = chain + [{'path': key, 'desc': str(node.get_editor_property('desc')),
                                   'class': node.get_class().get_name()}]
            edges = []
            for index, name, source in inputs(material, node):
                edges.append({'input_index': index, 'input_name': name,
                              'source_path': source.get_path_name() if source else None})
                if source:
                    dfs(source, paths[key] + [{'via_input': name, 'input_index': index}])
            all_edges[key] = edges
            active.remove(key)

        dfs(entry, [])
        original_chain, original_seen = [], set()
        cursor = entry
        while cursor is not None:
            key = cursor.get_path_name()
            original_chain.append(describe(cursor))
            if key in original_seen:
                original_chain.append({'cycle_detected': True})
                break
            original_seen.add(key)
            originals = [source for _, name, source in inputs(material, cursor) if name == 'Original']
            if len(originals) != 1:
                break
            cursor = originals[0]
        global_nodes = [node for node in nodes if str(node.get_editor_property('desc')) == GLOBAL + 'Wrap' + label]
        global_originals = []
        for wrapper in global_nodes:
            for _, name, source in inputs(material, wrapper):
                if name == 'Original':
                    global_originals.append({'wrapper': wrapper.get_path_name(), 'source': describe(source),
                                             'source_is_exact_style_target': source == target,
                                             'wrapper_reachable_from_property': wrapper.get_path_name() in visited})
        rows[label] = {
            'root': describe(entry),
            'root_output_name': str(L.get_material_property_input_node_output_name(material, prop)),
            'style_target_count': len(targets[label]), 'style_target': describe(target),
            'style_reachable_from_active_property': bool(target and target.get_path_name() in visited),
            'active_path_to_style': paths.get(target.get_path_name(), []) if target else [],
            'original_input_chain': original_chain, 'global_wrapper_original_inputs': global_originals,
            'graph_cycle_edges': cycles, 'reachable_expression_count': len(visited),
            'reachable_expression_paths': sorted(visited),
        }
        if target:
            rows[label]['style_actual_inputs'] = [
                {'input_index': index, 'input_name': name, 'source': describe(source)}
                for index, name, source in inputs(material, target)]
    inventory = [describe(node) for node in nodes if
                 str(node.get_editor_property('desc')).startswith((STYLE, GLOBAL, 'AS_GROUND_REFINEMENT_V1:', 'AS_ENVIRONMENT_QUALITY_V1:'))]
    return {'material': material.get_path_name(), 'expression_count': len(nodes), 'properties': rows,
            'owned_inventory': inventory, 'reachable_edges': all_edges,
            'all_three_style_outputs_reachable': all(row['style_reachable_from_active_property'] for row in rows.values()),
            'graph_cycle_free': not any(row['graph_cycle_edges'] for row in rows.values())}


def main():
    rows, errors = [], []
    for path in MATERIALS:
        try:
            material = unreal.load_asset(path)
            if not isinstance(material, unreal.Material):
                raise RuntimeError('Required material missing')
            rows.append(inspect(material))
        except Exception as exc:
            errors.append({'material': path, 'error': str(exc)})
    report = {'schema_version': 1, 'valid': len(rows) == 2 and not errors and
              all(row['all_three_style_outputs_reachable'] and row['graph_cycle_free'] for row in rows),
              'materials': rows, 'errors': errors, 'asset_mutations': False,
              'recompile_requested': False, 'asset_save_requested': False,
              'scope': 'Native expression graph only. Reachability is not evaluated branch weight, runtime MID value, shader execution or art acceptance.'}
    destination = ROOT / 'Saved/PresentationValidation/material_graph_reachability.json'
    command = unreal.SystemLibrary.get_command_line()
    match = re.search(r'(?:^|\s)-AftershockGraphTraceOutput=(?:"([^"]+)"|(\S+))', command)
    if match:
        destination = Path(match.group(1) or match.group(2))
        if not destination.is_absolute():
            destination = ROOT / destination
    destination = destination.resolve()
    if not destination.is_relative_to(ROOT / 'Saved'):
        raise RuntimeError('Diagnostic destination must remain under project Saved')
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    unreal.log_warning('AFTERSHOCK_MATERIAL_GRAPH_REACHABILITY ' + json.dumps({
        'valid': report['valid'], 'materials': len(rows), 'errors': errors,
        'report': str(destination), 'asset_mutations': False}))
    return report


if __name__ == '__main__':
    main()
